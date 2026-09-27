#include "aimp/CharacterActionProbeDiagnostics.h"

#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace aimp::character_action_probe {
namespace {

using CharacterControlUpdateFn = void(__thiscall*)(void*, std::uint32_t, float);
using ShootingManagerTickFn = void(__fastcall*)(void*);
using ShootingRequestReleaseFn = void(__fastcall*)(void*);
using ShootingRequestCancelFn = void(__fastcall*)(void*);

constexpr std::uintptr_t kCharacterControlOffset = 0x33E0U;
constexpr std::uintptr_t kCharacterGuard2FaOffset = 0x2FAU;
constexpr std::uintptr_t kCharacterComponent388Offset = 0x388U;
constexpr std::uintptr_t kCharacterContext34D4Offset = 0x34D4U;
constexpr std::uintptr_t kCharacterAuthorizationAOffset = 0x34F8U;
constexpr std::uintptr_t kCharacterAuthorizationBOffset = 0x34FCU;

constexpr std::uintptr_t kShootingManagerOwnerOffset = 0x00U;
constexpr std::uintptr_t kShootingManagerStateObjectOffset = 0x04U;
constexpr std::uintptr_t kShootingManagerActiveCountOffset = 0x0CU;
constexpr std::uintptr_t kShootingManagerCurrentStateOffset = 0x24U;
constexpr std::uintptr_t kShootingManagerActiveOffset = 0x28U;
constexpr std::uintptr_t kShootingManagerTransitionOffset = 0x2CU;
constexpr std::uintptr_t kShootingManagerState3CallbackOffset = 0x64U;
constexpr std::uintptr_t kShootingManagerState5CallbackOffset = 0x6CU;
constexpr std::uintptr_t kShootingManagerState5ExitCallbackOffset = 0xACU;
constexpr std::uintptr_t kShootingManagerRequestedStateOffset = 0xB8U;

constexpr std::uintptr_t kShootingStateOwnerOffset = 0x24U;
constexpr std::uintptr_t kShootingStateFlagsOffset = 0x4CU;

constexpr std::uintptr_t kShootingRequestStateOffset = 0x0CU;
constexpr std::uintptr_t kShootingRequestTypeOffset = 0x10U;
constexpr std::uintptr_t kShootingRequestManagerOffset = 0x14U;
constexpr std::uintptr_t kShootingRequestBudgetOffset = 0x2CU;
constexpr std::uintptr_t kShootingRequestFlag32Offset = 0x32U;
constexpr std::uintptr_t kShootingRequestFlag33Offset = 0x33U;

constexpr std::uintptr_t kEquipmentPrimaryOffset = 0xA2U;
constexpr std::uintptr_t kEquipmentOwnerOffset = 0x1528U;
constexpr std::uintptr_t kEquipmentContext153COffset = 0x153CU;
constexpr std::uintptr_t kEquipmentNpcFireAuthorizationOffset = 0x1575U;

constexpr std::uintptr_t kCharacterControlUpdateRva = 0x003342A0U;
constexpr std::uintptr_t kCharacterControlLoopRva = 0x00334690U;
constexpr std::size_t kCharacterControlLoopBytes = 0x290U;
constexpr std::uintptr_t kShootingManagerTickRva = 0x00073570U;
constexpr std::size_t kCharacterControlUpdateBytes = 0x240U;
constexpr std::uintptr_t kState3CallbackRva = 0x0006FA00U;
constexpr std::uintptr_t kState5CallbackRva = 0x00072B90U;
constexpr std::uintptr_t kState5ExitCallbackRva = 0x0006B2F0U;

// CATHODE::NPC_TriggerShootRequest tables identified from FUN_00605B60 on
// the exact certified retail build. These are data-table RVAs, never call
// targets. The inspector below only reads them.
constexpr std::uintptr_t kNpcTriggerShootRequestInterfaceTableRva = 0x00E93B18U;
constexpr std::uintptr_t kNpcTriggerShootRequestImplementationTableRva = 0x00E93EB0U;
constexpr std::size_t kNpcTriggerShootRequestTableEntries = 32U;
constexpr std::size_t kNpcTriggerShootRequestCodeWindowBytes = 160U;

// Sibling CATHODE action-node tables from the exact same retail build.
// These are data tables installed by their constructors; none of these RVAs is
// treated as a callable entry point by the comparator.
constexpr std::uintptr_t kNpcTriggerAimRequestInterfaceTableRva = 0x00E93158U;
constexpr std::uintptr_t kNpcTriggerAimRequestImplementationTableRva = 0x00E934F0U;
constexpr std::uintptr_t kForceReloadWeaponInterfaceTableRva = 0x00E902C0U;
constexpr std::uintptr_t kForceReloadWeaponImplementationTableRva = 0x00E90178U;
constexpr std::uintptr_t kForceReloadWeaponExecutorRva = 0x00203290U;
constexpr std::size_t kActionNodeCompareEntries = 32U;
constexpr std::size_t kActionNodeCompareCodeBytes = 192U;

constexpr std::uint32_t kTraceMaximumFrames = 480U;
constexpr std::uint64_t kTraceMaximumMilliseconds = 8000U;

std::atomic<std::uintptr_t> gObservedControl{0};
std::atomic<std::uintptr_t> gObservedManager{0};
std::atomic<std::uint64_t> gCharacterControlCalls{0};
std::atomic<std::uint64_t> gManagerTickCalls{0};
std::atomic<void*> gCharacterControlTarget{nullptr};
std::atomic<void*> gManagerTickTarget{nullptr};
std::atomic<bool> gObservationHooksInstalled{false};

struct Sample final {
    bool readable{};
    std::uint8_t characterGuard2Fa{};
    std::uintptr_t characterComponent388{};
    std::uintptr_t characterContext34D4{};
    std::uintptr_t authorizationA{};
    std::uintptr_t authorizationB{};
    std::uintptr_t managerOwner{};
    std::uintptr_t stateObject{};
    std::uintptr_t stateOwner{};
    std::int32_t activeCount{-1};
    std::int32_t currentState{-1};
    std::uint8_t machineActive{};
    std::int32_t transitionState{-1};
    std::int32_t requestedState{-1};
    std::uint8_t stateFlags{};
    std::int32_t requestState{-1};
    std::int32_t requestType{-1};
    std::uintptr_t requestManager{};
    std::int32_t requestBudget{-1};
    std::uint8_t requestFlag32{};
    std::uint8_t requestFlag33{};
    std::uint8_t primaryA2{};
    std::uintptr_t equipmentOwner{};
    std::uint8_t equipmentContext153C{};
    std::uint8_t equipmentNpcFireAuthorization{};
    std::uintptr_t state3Callback{};
    std::uintptr_t state5Callback{};
    std::uintptr_t state5ExitCallback{};
};

struct TraceState final {
    bool active{};
    bool complete{};
    bool retainedForCancel{};
    bool shutdown{};
    std::uintptr_t character{};
    std::uintptr_t control{};
    std::uintptr_t manager{};
    std::uintptr_t equipment{};
    std::uint64_t generation{};
    std::uint64_t startedTick{};
    std::uint32_t frames{};
    NativeShootingRequestRef request{};
    const void* releaseAddress{};
    const void* cancelAddress{};
    Diagnostics* diagnostics{};
    Sample first{};
    Sample last{};
    bool hasFirst{};
    std::uint32_t currentStateTransitions{};
    std::uint32_t requestedStateTransitions{};
    std::uint32_t requestStateTransitions{};
    std::uint32_t requestBudgetTransitions{};
    std::uint32_t primaryTransitions{};
    std::uint32_t state3Frames{};
    std::uint32_t state5Frames{};
    bool everRequested3{};
    bool everCurrent3{};
    bool everRequested5{};
    bool everCurrent5{};
    bool everPrimary{};
    std::string finalClassification;
};

std::mutex gMutex;
TraceState gTrace;

bool AddAddress(
    const std::uintptr_t base,
    const std::uintptr_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 ||
        base > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        result = 0;
        return false;
    }
    result = base + offset;
    return true;
}

template <typename T>
bool ReadAt(
    const std::uintptr_t base,
    const std::uintptr_t offset,
    T& value) noexcept {
    std::uintptr_t address = 0;
    if (!AddAddress(base, offset, address)) {
        return false;
    }
    const auto observed = crash_guard::SafeRead<T>(
        reinterpret_cast<const void*>(address));
    if (!observed.has_value()) {
        return false;
    }
    value = *observed;
    return true;
}

struct LoadedImageLayout final {
    std::uintptr_t imageStart{};
    std::uintptr_t imageEnd{};
    std::uintptr_t textStart{};
    std::uintptr_t textEnd{};
    std::uint32_t sizeOfImage{};
};

bool ReadLoadedImageLayout(
    const std::uintptr_t moduleBase,
    LoadedImageLayout& layout,
    std::string& error) noexcept {
    layout = {};
    if (moduleBase < 0x10000U) {
        error = "invalid module base";
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!crash_guard::TryCopy(
            &dos,
            reinterpret_cast<const void*>(moduleBase),
            sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) {
        error = "loaded module DOS header is not readable/valid";
        return false;
    }

    std::uintptr_t ntAddress = 0;
    if (!AddAddress(
            moduleBase,
            static_cast<std::uintptr_t>(dos.e_lfanew),
            ntAddress)) {
        error = "loaded module NT header address overflow";
        return false;
    }

    IMAGE_NT_HEADERS32 nt{};
    if (!crash_guard::TryCopy(
            &nt,
            reinterpret_cast<const void*>(ntAddress),
            sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage == 0) {
        error = "loaded module PE32 headers are not readable/valid";
        return false;
    }

    std::uintptr_t imageEnd = 0;
    if (!AddAddress(moduleBase, nt.OptionalHeader.SizeOfImage, imageEnd)) {
        error = "loaded module image range overflow";
        return false;
    }

    const auto sectionTable = ntAddress + sizeof(DWORD) +
        sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    IMAGE_SECTION_HEADER text{};
    bool foundText = false;
    for (std::uint16_t index = 0;
         index < nt.FileHeader.NumberOfSections;
         ++index) {
        IMAGE_SECTION_HEADER section{};
        const auto sectionAddress = sectionTable +
            static_cast<std::uintptr_t>(index) * sizeof(section);
        if (!crash_guard::TryCopy(
                &section,
                reinterpret_cast<const void*>(sectionAddress),
                sizeof(section))) {
            error = "loaded module section table is not fully readable";
            return false;
        }
        if (std::memcmp(section.Name, ".text", 5) == 0) {
            text = section;
            foundText = true;
            break;
        }
    }
    if (!foundText) {
        error = "loaded module has no readable .text section";
        return false;
    }

    const auto textBytes = static_cast<std::uintptr_t>((std::max)(
        text.Misc.VirtualSize,
        text.SizeOfRawData));
    std::uintptr_t textStart = 0;
    std::uintptr_t textEnd = 0;
    if (!AddAddress(moduleBase, text.VirtualAddress, textStart) ||
        !AddAddress(textStart, textBytes, textEnd) ||
        textStart < moduleBase || textEnd > imageEnd || textStart >= textEnd) {
        error = "loaded module .text range is invalid";
        return false;
    }

    layout.imageStart = moduleBase;
    layout.imageEnd = imageEnd;
    layout.textStart = textStart;
    layout.textEnd = textEnd;
    layout.sizeOfImage = nt.OptionalHeader.SizeOfImage;
    error.clear();
    return true;
}

bool InRange(
    const std::uintptr_t value,
    const std::uintptr_t begin,
    const std::uintptr_t end) noexcept {
    return value >= begin && value < end;
}

std::string HexValue(const std::uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex
           << std::setw(8) << std::setfill('0')
           << static_cast<std::uint64_t>(value);
    return stream.str();
}

std::string HexBytes(const std::vector<std::byte>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(bytes.size() * 2U);
    for (const auto value : bytes) {
        const auto byte = std::to_integer<std::uint8_t>(value);
        output.push_back(kHex[(byte >> 4U) & 0x0FU]);
        output.push_back(kHex[byte & 0x0FU]);
    }
    return output;
}

const char* KnownShootRequestTarget(const std::uintptr_t rva) noexcept {
    switch (rva) {
    case 0x000707A0U:
        return "FUN_004707A0_create_shooting_request";
    case 0x00071D70U:
        return "FUN_00471D70_activate_without_replacement";
    case 0x00071E00U:
        return "FUN_00471E00_activate_with_replacement";
    case 0x00071E90U:
        return "FUN_00471E90_cancel_request";
    case 0x0000D770U:
        return "FUN_0040D770_release_shared_ref";
    case 0x00205B60U:
        return "FUN_00605B60_NPC_TriggerShootRequest_constructor";
    case 0x004C96C0U:
        return "FUN_008C96C0_NPC_TriggerShootRequest_descriptor";
    default:
        return nullptr;
    }
}

struct DirectCallScan final {
    std::string targets;
    std::string knownMatches;
    std::uint32_t count{};
};

DirectCallScan ScanDirectE8Calls(
    const LoadedImageLayout& layout,
    const std::uintptr_t functionAddress,
    const std::vector<std::byte>& code) {
    DirectCallScan result{};
    std::ostringstream targets;
    std::ostringstream known;
    bool firstTarget = true;
    bool firstKnown = true;
    for (std::size_t index = 0; index + 5U <= code.size(); ++index) {
        if (std::to_integer<std::uint8_t>(code[index]) != 0xE8U) {
            continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(
            &displacement,
            code.data() + index + 1U,
            sizeof(displacement));
        const auto site = functionAddress + index;
        const auto destinationSigned =
            static_cast<std::intptr_t>(site + 5U) + displacement;
        if (destinationSigned <= 0) {
            continue;
        }
        const auto destination = static_cast<std::uintptr_t>(destinationSigned);
        if (!InRange(destination, layout.textStart, layout.textEnd)) {
            continue;
        }
        if (result.count >= 16U) {
            break;
        }
        const auto rva = destination - layout.imageStart;
        if (!firstTarget) {
            targets << ',';
        }
        firstTarget = false;
        targets << HexValue(rva);
        ++result.count;
        if (const auto* label = KnownShootRequestTarget(rva); label != nullptr) {
            if (!firstKnown) {
                known << ',';
            }
            firstKnown = false;
            known << HexValue(rva) << '=' << label;
        }
    }
    result.targets = targets.str();
    result.knownMatches = known.str();
    return result;
}

struct TableSnapshot final {
    std::uint32_t readableEntries{};
    std::uint32_t textCandidates{};
    std::string preview;
};

TableSnapshot InspectRuntimeTable(
    const char* tableName,
    const std::uintptr_t tableRva,
    const LoadedImageLayout& layout,
    Diagnostics& diagnostics) {
    TableSnapshot snapshot{};
    const auto tableAddress = layout.imageStart + tableRva;
    std::ostringstream preview;
    for (std::size_t index = 0;
         index < kNpcTriggerShootRequestTableEntries;
         ++index) {
        const auto entryAddress = tableAddress + index * sizeof(std::uintptr_t);
        const auto entry = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(entryAddress));
        if (!entry.has_value()) {
            if (index != 0) {
                preview << ';';
            }
            preview << index << "=<unreadable>";
            continue;
        }
        ++snapshot.readableEntries;
        const bool inText = InRange(*entry, layout.textStart, layout.textEnd);
        if (index != 0) {
            preview << ';';
        }
        preview << index << '=' << HexValue(*entry -
            (InRange(*entry, layout.imageStart, layout.imageEnd)
                ? layout.imageStart
                : 0U));
        preview << (inText ? "[text]" : "[nontext]");
        if (!inText) {
            continue;
        }

        ++snapshot.textCandidates;
        const auto available = static_cast<std::size_t>(layout.textEnd - *entry);
        const auto captureBytes = (std::min)(
            kNpcTriggerShootRequestCodeWindowBytes,
            available);
        std::vector<std::byte> code(captureBytes);
        if (captureBytes == 0 || !crash_guard::TryCopy(
                code.data(),
                reinterpret_cast<const void*>(*entry),
                code.size())) {
            diagnostics.Log(
                "warning",
                "npc_trigger_shoot_request_candidate_unreadable",
                "A table entry points into .text but its bounded code window could not be copied.",
                {{"table", tableName},
                 {"tableRva", HexValue(tableRva)},
                 {"index", Diagnostics::Unsigned(index), true},
                 {"entryAddress", Diagnostics::Pointer(reinterpret_cast<const void*>(entryAddress))},
                 {"target", Diagnostics::Pointer(reinterpret_cast<const void*>(*entry))},
                 {"targetRva", HexValue(*entry - layout.imageStart)}});
            continue;
        }

        const auto calls = ScanDirectE8Calls(layout, *entry, code);
        diagnostics.Log(
            "info",
            "npc_trigger_shoot_request_candidate",
            "Read-only candidate callback snapshot. Direct-call targets are a bounded E8 heuristic; no entry was invoked.",
            {{"table", tableName},
             {"tableRva", HexValue(tableRva)},
             {"index", Diagnostics::Unsigned(index), true},
             {"entryAddress", Diagnostics::Pointer(reinterpret_cast<const void*>(entryAddress))},
             {"target", Diagnostics::Pointer(reinterpret_cast<const void*>(*entry))},
             {"targetRva", HexValue(*entry - layout.imageStart)},
             {"capturedBytes", Diagnostics::Unsigned(code.size()), true},
             {"codeHex", HexBytes(code)},
             {"directE8TargetsRva", calls.targets},
             {"knownDirectE8Matches", calls.knownMatches},
             {"directE8Count", Diagnostics::Unsigned(calls.count), true},
             {"readOnly", Diagnostics::Boolean(true), true}});
    }
    snapshot.preview = preview.str();
    diagnostics.Log(
        "info",
        "npc_trigger_shoot_request_table",
        "Read-only snapshot of one NPC_TriggerShootRequest runtime table. Entries were read only; no target was invoked.",
        {{"table", tableName},
         {"tableRva", HexValue(tableRva)},
         {"tableAddress", Diagnostics::Pointer(reinterpret_cast<const void*>(tableAddress))},
         {"entryBudget", Diagnostics::Unsigned(kNpcTriggerShootRequestTableEntries), true},
         {"readableEntries", Diagnostics::Unsigned(snapshot.readableEntries), true},
         {"textCandidates", Diagnostics::Unsigned(snapshot.textCandidates), true},
         {"entries", snapshot.preview},
         {"readOnly", Diagnostics::Boolean(true), true}});
    return snapshot;
}

struct ActionNodeCompareEntry final {
    bool readable{};
    bool inText{};
    std::uintptr_t target{};
    std::uintptr_t rva{};
    bool hasNearJump{};
    std::uintptr_t nearJumpRva{};
    bool directCallsReloadExecutor{};
};

using ActionNodeCompareTable =
    std::array<ActionNodeCompareEntry, kActionNodeCompareEntries>;

bool ReadBoundedCode(
    const LoadedImageLayout& layout,
    const std::uintptr_t target,
    const std::size_t budget,
    std::vector<std::byte>& code) noexcept {
    code.clear();
    if (!InRange(target, layout.textStart, layout.textEnd)) {
        return false;
    }
    const auto available = static_cast<std::size_t>(layout.textEnd - target);
    const auto bytes = (std::min)(budget, available);
    if (bytes == 0U) {
        return false;
    }
    code.resize(bytes);
    if (!crash_guard::TryCopy(
            code.data(),
            reinterpret_cast<const void*>(target),
            code.size())) {
        code.clear();
        return false;
    }
    return true;
}

bool FindFirstNearJumpTarget(
    const LoadedImageLayout& layout,
    const std::uintptr_t functionAddress,
    const std::vector<std::byte>& code,
    std::uintptr_t& destinationRva) noexcept {
    destinationRva = 0;
    // MSVC adjustor thunks commonly perform a tiny ECX adjustment followed by
    // E9 rel32 (for example the Shoot interface destructor thunk observed at
    // RVA 0x00205C50). Restrict this heuristic to the first 12 bytes so it
    // cannot be mistaken for an arbitrary later branch in the function body.
    const auto limit = (std::min)(std::size_t{12U}, code.size());
    for (std::size_t index = 0; index + 5U <= limit; ++index) {
        if (std::to_integer<std::uint8_t>(code[index]) != 0xE9U) {
            continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(
            &displacement,
            code.data() + index + 1U,
            sizeof(displacement));
        const auto site = functionAddress + index;
        const auto signedDestination =
            static_cast<std::intptr_t>(site + 5U) + displacement;
        if (signedDestination <= 0) {
            return false;
        }
        const auto destination =
            static_cast<std::uintptr_t>(signedDestination);
        if (!InRange(destination, layout.textStart, layout.textEnd)) {
            return false;
        }
        destinationRva = destination - layout.imageStart;
        return true;
    }
    return false;
}

bool CodeDirectCallsRva(
    const LoadedImageLayout& layout,
    const std::uintptr_t functionAddress,
    const std::vector<std::byte>& code,
    const std::uintptr_t wantedRva) noexcept {
    for (std::size_t index = 0; index + 5U <= code.size(); ++index) {
        if (std::to_integer<std::uint8_t>(code[index]) != 0xE8U) {
            continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(
            &displacement,
            code.data() + index + 1U,
            sizeof(displacement));
        const auto site = functionAddress + index;
        const auto signedDestination =
            static_cast<std::intptr_t>(site + 5U) + displacement;
        if (signedDestination <= 0) {
            continue;
        }
        const auto destination =
            static_cast<std::uintptr_t>(signedDestination);
        if (InRange(destination, layout.textStart, layout.textEnd) &&
            destination - layout.imageStart == wantedRva) {
            return true;
        }
    }
    return false;
}

ActionNodeCompareTable ReadActionNodeCompareTable(
    const std::uintptr_t tableRva,
    const LoadedImageLayout& layout) {
    ActionNodeCompareTable table{};
    const auto tableAddress = layout.imageStart + tableRva;
    for (std::size_t index = 0; index < table.size(); ++index) {
        auto& entry = table[index];
        const auto entryAddress = tableAddress + index * sizeof(std::uintptr_t);
        const auto value = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(entryAddress));
        if (!value.has_value()) {
            continue;
        }
        entry.readable = true;
        entry.target = *value;
        entry.inText = InRange(entry.target, layout.textStart, layout.textEnd);
        if (InRange(entry.target, layout.imageStart, layout.imageEnd)) {
            entry.rva = entry.target - layout.imageStart;
        }
        if (!entry.inText) {
            continue;
        }
        std::vector<std::byte> code;
        if (!ReadBoundedCode(
                layout,
                entry.target,
                kActionNodeCompareCodeBytes,
                code)) {
            continue;
        }
        entry.hasNearJump = FindFirstNearJumpTarget(
            layout,
            entry.target,
            code,
            entry.nearJumpRva);
        entry.directCallsReloadExecutor = CodeDirectCallsRva(
            layout,
            entry.target,
            code,
            kForceReloadWeaponExecutorRva);
    }
    return table;
}

std::string CompareEntryRva(const ActionNodeCompareEntry& entry) {
    if (!entry.readable) {
        return "<unreadable>";
    }
    if (entry.rva == 0U) {
        return "<outside-image>";
    }
    return HexValue(entry.rva);
}

std::string CompareEntryJumpRva(const ActionNodeCompareEntry& entry) {
    return entry.hasNearJump ? HexValue(entry.nearJumpRva) : std::string{};
}

const char* ReloadAnchorKind(const ActionNodeCompareEntry& entry) noexcept {
    if (!entry.readable || !entry.inText) {
        return nullptr;
    }
    if (entry.rva == kForceReloadWeaponExecutorRva) {
        return "direct_vtable_target";
    }
    if (entry.hasNearJump &&
        entry.nearJumpRva == kForceReloadWeaponExecutorRva) {
        return "one_level_near_jump_thunk";
    }
    if (entry.directCallsReloadExecutor) {
        return "bounded_direct_E8_call";
    }
    return nullptr;
}

std::string CaptureCompareCodeHex(
    const LoadedImageLayout& layout,
    const ActionNodeCompareEntry& entry) {
    if (!entry.readable || !entry.inText) {
        return {};
    }
    std::vector<std::byte> code;
    if (!ReadBoundedCode(
            layout,
            entry.target,
            kActionNodeCompareCodeBytes,
            code)) {
        return {};
    }
    return HexBytes(code);
}

std::string CaptureCompareDirectCalls(
    const LoadedImageLayout& layout,
    const ActionNodeCompareEntry& entry) {
    if (!entry.readable || !entry.inText) {
        return {};
    }
    std::vector<std::byte> code;
    if (!ReadBoundedCode(
            layout,
            entry.target,
            kActionNodeCompareCodeBytes,
            code)) {
        return {};
    }
    return ScanDirectE8Calls(layout, entry.target, code).targets;
}

struct CompareRoleSummary final {
    std::uint32_t readableTriplets{};
    std::uint32_t identicalTriplets{};
    std::uint32_t differingTriplets{};
    std::uint32_t reloadAnchorCandidates{};
};

CompareRoleSummary CompareActionNodeRole(
    const char* role,
    const std::uintptr_t aimTableRva,
    const std::uintptr_t shootTableRva,
    const std::uintptr_t reloadTableRva,
    const LoadedImageLayout& layout,
    Diagnostics& diagnostics) {
    const auto aim = ReadActionNodeCompareTable(aimTableRva, layout);
    const auto shoot = ReadActionNodeCompareTable(shootTableRva, layout);
    const auto reload = ReadActionNodeCompareTable(reloadTableRva, layout);
    CompareRoleSummary summary{};

    for (std::size_t index = 0; index < kActionNodeCompareEntries; ++index) {
        const auto& a = aim[index];
        const auto& s = shoot[index];
        const auto& r = reload[index];
        const bool tripletReadable = a.readable && s.readable && r.readable;
        if (tripletReadable) {
            ++summary.readableTriplets;
        }
        const bool allSame = tripletReadable &&
            a.target == s.target && s.target == r.target;
        if (allSame) {
            ++summary.identicalTriplets;
        } else if (tripletReadable) {
            ++summary.differingTriplets;
        }

        const auto* anchorKind = ReloadAnchorKind(r);
        if (anchorKind != nullptr) {
            ++summary.reloadAnchorCandidates;
        }

        // Keep the log bounded and useful: emit every differing slot plus any
        // slot that independently matches the known reload execution function.
        if (allSame && anchorKind == nullptr) {
            continue;
        }

        diagnostics.Log(
            "info",
            "cathode_action_node_vtable_slot",
            "Read-only Aim/Shoot/Reload vtable comparison. No slot was invoked.",
            {{"role", role},
             {"index", Diagnostics::Unsigned(index), true},
             {"aimRva", CompareEntryRva(a)},
             {"shootRva", CompareEntryRva(s)},
             {"reloadRva", CompareEntryRva(r)},
             {"aimNearJumpRva", CompareEntryJumpRva(a)},
             {"shootNearJumpRva", CompareEntryJumpRva(s)},
             {"reloadNearJumpRva", CompareEntryJumpRva(r)},
             {"sameAimShoot", Diagnostics::Boolean(
                 a.readable && s.readable && a.target == s.target), true},
             {"sameShootReload", Diagnostics::Boolean(
                 s.readable && r.readable && s.target == r.target), true},
             {"sameAimReload", Diagnostics::Boolean(
                 a.readable && r.readable && a.target == r.target), true},
             {"reloadCallsKnownExecutor", Diagnostics::Boolean(
                 r.directCallsReloadExecutor), true},
             {"reloadAnchorKind", anchorKind == nullptr ? "" : anchorKind},
             {"readOnly", Diagnostics::Boolean(true), true}});

        if (anchorKind == nullptr) {
            continue;
        }

        diagnostics.Log(
            "info",
            "cathode_action_node_execution_slot_candidate",
            "The Reload entry at this slot reaches the already-known FUN_00603290 executor. The Aim/Shoot entries at the same role/index are captured as structural execution-slot candidates only; they are not called.",
            {{"role", role},
             {"index", Diagnostics::Unsigned(index), true},
             {"anchorKind", anchorKind},
             {"knownReloadExecutorRva", HexValue(kForceReloadWeaponExecutorRva)},
             {"aimRva", CompareEntryRva(a)},
             {"shootRva", CompareEntryRva(s)},
             {"reloadRva", CompareEntryRva(r)},
             {"aimNearJumpRva", CompareEntryJumpRva(a)},
             {"shootNearJumpRva", CompareEntryJumpRva(s)},
             {"reloadNearJumpRva", CompareEntryJumpRva(r)},
             {"aimCodeHex", CaptureCompareCodeHex(layout, a)},
             {"shootCodeHex", CaptureCompareCodeHex(layout, s)},
             {"reloadCodeHex", CaptureCompareCodeHex(layout, r)},
             {"aimDirectE8TargetsRva", CaptureCompareDirectCalls(layout, a)},
             {"shootDirectE8TargetsRva", CaptureCompareDirectCalls(layout, s)},
             {"reloadDirectE8TargetsRva", CaptureCompareDirectCalls(layout, r)},
             {"readOnly", Diagnostics::Boolean(true), true}});
    }

    diagnostics.Log(
        "info",
        "cathode_action_node_vtable_role_summary",
        "Read-only summary for one Aim/Shoot/Reload table role.",
        {{"role", role},
         {"aimTableRva", HexValue(aimTableRva)},
         {"shootTableRva", HexValue(shootTableRva)},
         {"reloadTableRva", HexValue(reloadTableRva)},
         {"entryBudget", Diagnostics::Unsigned(kActionNodeCompareEntries), true},
         {"readableTriplets", Diagnostics::Unsigned(summary.readableTriplets), true},
         {"identicalTriplets", Diagnostics::Unsigned(summary.identicalTriplets), true},
         {"differingTriplets", Diagnostics::Unsigned(summary.differingTriplets), true},
         {"reloadAnchorCandidates", Diagnostics::Unsigned(summary.reloadAnchorCandidates), true},
         {"readOnly", Diagnostics::Boolean(true), true}});
    return summary;
}

Sample ReadSample(const TraceState& trace) noexcept {
    Sample sample{};
    if (trace.character == 0 || trace.manager == 0 ||
        trace.equipment == 0 || trace.request.object == nullptr) {
        return sample;
    }

    const auto request = reinterpret_cast<std::uintptr_t>(trace.request.object);
    bool ok = true;
    ok = ReadAt(trace.character, kCharacterGuard2FaOffset, sample.characterGuard2Fa) && ok;
    ok = ReadAt(trace.character, kCharacterComponent388Offset, sample.characterComponent388) && ok;
    ok = ReadAt(trace.character, kCharacterContext34D4Offset, sample.characterContext34D4) && ok;
    ok = ReadAt(trace.character, kCharacterAuthorizationAOffset, sample.authorizationA) && ok;
    ok = ReadAt(trace.character, kCharacterAuthorizationBOffset, sample.authorizationB) && ok;

    ok = ReadAt(trace.manager, kShootingManagerOwnerOffset, sample.managerOwner) && ok;
    ok = ReadAt(trace.manager, kShootingManagerStateObjectOffset, sample.stateObject) && ok;
    ok = ReadAt(trace.manager, kShootingManagerActiveCountOffset, sample.activeCount) && ok;
    ok = ReadAt(trace.manager, kShootingManagerCurrentStateOffset, sample.currentState) && ok;
    ok = ReadAt(trace.manager, kShootingManagerActiveOffset, sample.machineActive) && ok;
    ok = ReadAt(trace.manager, kShootingManagerTransitionOffset, sample.transitionState) && ok;
    ok = ReadAt(trace.manager, kShootingManagerRequestedStateOffset, sample.requestedState) && ok;
    ok = ReadAt(trace.manager, kShootingManagerState3CallbackOffset, sample.state3Callback) && ok;
    ok = ReadAt(trace.manager, kShootingManagerState5CallbackOffset, sample.state5Callback) && ok;
    ok = ReadAt(trace.manager, kShootingManagerState5ExitCallbackOffset, sample.state5ExitCallback) && ok;

    if (sample.stateObject != 0) {
        ok = ReadAt(sample.stateObject, kShootingStateOwnerOffset, sample.stateOwner) && ok;
        ok = ReadAt(sample.stateObject, kShootingStateFlagsOffset, sample.stateFlags) && ok;
    } else {
        ok = false;
    }

    ok = ReadAt(request, kShootingRequestStateOffset, sample.requestState) && ok;
    ok = ReadAt(request, kShootingRequestTypeOffset, sample.requestType) && ok;
    ok = ReadAt(request, kShootingRequestManagerOffset, sample.requestManager) && ok;
    ok = ReadAt(request, kShootingRequestBudgetOffset, sample.requestBudget) && ok;
    ok = ReadAt(request, kShootingRequestFlag32Offset, sample.requestFlag32) && ok;
    ok = ReadAt(request, kShootingRequestFlag33Offset, sample.requestFlag33) && ok;

    ok = ReadAt(trace.equipment, kEquipmentPrimaryOffset, sample.primaryA2) && ok;
    ok = ReadAt(trace.equipment, kEquipmentOwnerOffset, sample.equipmentOwner) && ok;
    ok = ReadAt(trace.equipment, kEquipmentContext153COffset, sample.equipmentContext153C) && ok;
    ok = ReadAt(trace.equipment, kEquipmentNpcFireAuthorizationOffset, sample.equipmentNpcFireAuthorization) && ok;

    sample.readable = ok;
    return sample;
}

std::string Classification(const TraceState& trace) {
    const auto& last = trace.last;
    const auto controlCalls = gCharacterControlCalls.load(std::memory_order_acquire);
    const auto managerCalls = gManagerTickCalls.load(std::memory_order_acquire);

    if (!last.readable) {
        return "trace_memory_became_unreadable";
    }
    if (controlCalls == 0 && gObservationHooksInstalled.load(std::memory_order_acquire)) {
        return "character_control_update_not_reached";
    }
    if (last.characterGuard2Fa == 0) {
        return "character_guard_C_plus_2FA_false";
    }
    if (last.characterContext34D4 == 0) {
        return "character_context_C_plus_34D4_missing";
    }
    if (managerCalls == 0 && gObservationHooksInstalled.load(std::memory_order_acquire)) {
        return "shooting_manager_tick_not_reached";
    }
    if (last.machineActive == 0) {
        return "shooting_manager_machine_inactive";
    }
    if (!trace.everRequested3 && !trace.everCurrent3 && !trace.everCurrent5) {
        return "request_queued_but_manager_did_not_promote_state";
    }
    if (trace.everCurrent3 && !trace.everCurrent5) {
        if ((last.stateFlags & 0x0CU) != 0U) {
            return "state3_wait_blocking_S_plus_4C_bits_0C";
        }
        if (last.requestFlag32 == 0U && (last.stateFlags & 0x01U) == 0U) {
            return "state3_wait_missing_R32_or_S_flag_bit0_authorization";
        }
        if (last.characterComponent388 == 0) {
            return "state3_wait_component_C_plus_388_absent";
        }
        return "state3_wait_authorization_not_identified";
    }
    if (trace.everCurrent5 && !trace.everPrimary) {
        if (last.requestFlag33 == 0U &&
            last.equipmentNpcFireAuthorization == 0U) {
            return "state5_veto_R33_and_E_plus_1575_zero";
        }
        if (last.authorizationA == 0 || last.authorizationB == 0) {
            return "state5_veto_authorization_pair_C_plus_34F8_34FC_missing";
        }
        return "state5_veto_after_fire_decision_entry";
    }
    if (trace.everPrimary) {
        return "primary_A2_start_observed";
    }
    return "undetermined";
}

std::string Signed(const std::int32_t value) {
    return std::to_string(value);
}

std::string ByteValue(const std::uint8_t value) {
    return std::to_string(static_cast<unsigned int>(value));
}

bool CancelRequestProtected(
    const void* cancelAddress,
    void* requestObject) noexcept {
    if (cancelAddress == nullptr || requestObject == nullptr) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        const auto cancel = reinterpret_cast<ShootingRequestCancelFn>(
            const_cast<void*>(cancelAddress));
        cancel(requestObject);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    const auto cancel = reinterpret_cast<ShootingRequestCancelFn>(
        const_cast<void*>(cancelAddress));
    cancel(requestObject);
    return true;
#endif
}

void ReleaseHeldReference(TraceState& trace) noexcept {
    if (trace.request.control == nullptr || trace.releaseAddress == nullptr) {
        trace.request = {};
        return;
    }
#if defined(_MSC_VER)
    __try {
        const auto release = reinterpret_cast<ShootingRequestReleaseFn>(
            const_cast<void*>(trace.releaseAddress));
        release(&trace.request);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        trace.request = {};
    }
#else
    const auto release = reinterpret_cast<ShootingRequestReleaseFn>(
        const_cast<void*>(trace.releaseAddress));
    release(&trace.request);
#endif
    trace.request = {};
}

void LogTraceComplete(TraceState& trace, const char* reason) noexcept {
    try {
        trace.finalClassification = Classification(trace);
        auto* diagnostics = trace.diagnostics;
        if (diagnostics == nullptr) {
            return;
        }
        const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto state3Expected = moduleBase + kState3CallbackRva;
        const auto state5Expected = moduleBase + kState5CallbackRva;
        const auto state5ExitExpected = moduleBase + kState5ExitCallbackRva;

        diagnostics->Log(
        "info",
        "character_action_shoot_trace_complete",
        "Bounded ShootingRequest trace completed without forcing any manager state, predicate, A2 value, or native update.",
        {{"reason", reason},
         {"classification", trace.finalClassification},
         {"generation", Diagnostics::Unsigned(trace.generation), true},
         {"frames", Diagnostics::Unsigned(trace.frames), true},
         {"character", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.character))},
         {"control", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.control))},
         {"manager", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.manager))},
         {"equipment", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.equipment))},
         {"request", Diagnostics::Pointer(trace.request.object)},
         {"characterControlUpdateCalls", Diagnostics::Unsigned(gCharacterControlCalls.load(std::memory_order_acquire)), true},
         {"shootingManagerTickCalls", Diagnostics::Unsigned(gManagerTickCalls.load(std::memory_order_acquire)), true},
         {"callSiteCountersInstalled", Diagnostics::Boolean(gObservationHooksInstalled.load(std::memory_order_acquire)), true},
         {"initialReadable", Diagnostics::Boolean(trace.first.readable), true},
         {"initialManagerMachineActive", ByteValue(trace.first.machineActive), true},
         {"initialManagerCurrentState", Signed(trace.first.currentState), true},
         {"initialManagerRequestedState", Signed(trace.first.requestedState), true},
         {"initialManagerTransitionState", Signed(trace.first.transitionState), true},
         {"initialManagerActiveRequests", Signed(trace.first.activeCount), true},
         {"initialRequestState", Signed(trace.first.requestState), true},
         {"initialRequestBudget", Signed(trace.first.requestBudget), true},
         {"initialStateFlags4C", ByteValue(trace.first.stateFlags), true},
         {"initialPrimaryA2", ByteValue(trace.first.primaryA2), true},
         {"managerMachineActive", ByteValue(trace.last.machineActive), true},
         {"managerCurrentState", Signed(trace.last.currentState), true},
         {"managerRequestedState", Signed(trace.last.requestedState), true},
         {"managerTransitionState", Signed(trace.last.transitionState), true},
         {"managerActiveRequests", Signed(trace.last.activeCount), true},
         {"requestState", Signed(trace.last.requestState), true},
         {"requestBudget", Signed(trace.last.requestBudget), true},
         {"requestFlag32", ByteValue(trace.last.requestFlag32), true},
         {"requestFlag33", ByteValue(trace.last.requestFlag33), true},
         {"stateFlags4C", ByteValue(trace.last.stateFlags), true},
         {"primaryA2", ByteValue(trace.last.primaryA2), true},
         {"equipment153C", ByteValue(trace.last.equipmentContext153C), true},
         {"equipment1575", ByteValue(trace.last.equipmentNpcFireAuthorization), true},
         {"character2FA", ByteValue(trace.last.characterGuard2Fa), true},
         {"character388", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.last.characterComponent388))},
         {"character34D4", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.last.characterContext34D4))},
         {"authorization34F8", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.last.authorizationA))},
         {"authorization34FC", Diagnostics::Pointer(reinterpret_cast<const void*>(trace.last.authorizationB))},
         {"managerOwnerMatchesControl", Diagnostics::Boolean(trace.last.managerOwner == trace.control), true},
         {"stateOwnerMatchesControl", Diagnostics::Boolean(trace.last.stateOwner == trace.control), true},
         {"equipmentOwnerMatchesCharacter", Diagnostics::Boolean(trace.last.equipmentOwner == trace.character), true},
         {"requestManagerMatches", Diagnostics::Boolean(trace.last.requestManager == trace.manager), true},
         {"state3CallbackMatches", Diagnostics::Boolean(trace.last.state3Callback == state3Expected), true},
         {"state5CallbackMatches", Diagnostics::Boolean(trace.last.state5Callback == state5Expected), true},
         {"state5ExitCallbackMatches", Diagnostics::Boolean(trace.last.state5ExitCallback == state5ExitExpected), true},
         {"currentStateTransitions", Diagnostics::Unsigned(trace.currentStateTransitions), true},
         {"requestedStateTransitions", Diagnostics::Unsigned(trace.requestedStateTransitions), true},
         {"requestStateTransitions", Diagnostics::Unsigned(trace.requestStateTransitions), true},
         {"requestBudgetTransitions", Diagnostics::Unsigned(trace.requestBudgetTransitions), true},
         {"primaryTransitions", Diagnostics::Unsigned(trace.primaryTransitions), true},
         {"state3ObservedFrames", Diagnostics::Unsigned(trace.state3Frames), true},
         {"state5ObservedFrames", Diagnostics::Unsigned(trace.state5Frames), true},
         {"everRequestedState3", Diagnostics::Boolean(trace.everRequested3), true},
         {"everCurrentState3", Diagnostics::Boolean(trace.everCurrent3), true},
         {"everRequestedState5", Diagnostics::Boolean(trace.everRequested5), true},
         {"everCurrentState5", Diagnostics::Boolean(trace.everCurrent5), true},
         {"everPrimaryA2", Diagnostics::Boolean(trace.everPrimary), true},
         {"requestRetainedForCancel", Diagnostics::Boolean(trace.retainedForCancel), true}});
    } catch (...) {
    }
}
void FinalizeTraceLocked(TraceState& trace, const char* reason) noexcept {
    trace.active = false;
    trace.complete = true;
    gObservedControl.store(0, std::memory_order_release);
    gObservedManager.store(0, std::memory_order_release);

    const bool stillQueued = trace.last.activeCount > 0 &&
        trace.request.object != nullptr && trace.request.control != nullptr;
    trace.retainedForCancel = stillQueued;
    LogTraceComplete(trace, reason);
    if (!stillQueued) {
        ReleaseHeldReference(trace);
    }
}

std::vector<void*> FindDirectCalls(
    const std::uintptr_t caller,
    const std::size_t bytes,
    const std::uintptr_t target) {
    std::vector<std::byte> code(bytes);
    if (!crash_guard::TryCopy(code.data(), reinterpret_cast<const void*>(caller), bytes)) {
        return {};
    }
    std::vector<void*> matches;
    for (std::size_t index = 0; index + 5U <= code.size(); ++index) {
        if (std::to_integer<std::uint8_t>(code[index]) != 0xE8U) {
            continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, code.data() + index + 1U, sizeof(displacement));
        const auto site = caller + index;
        const auto destination = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(site + 5U) + displacement);
        if (destination == target) {
            matches.push_back(reinterpret_cast<void*>(site));
        }
    }
    return matches;
}

void __fastcall CharacterControlUpdateDetour(
    void* control,
    void*,
    const std::uint32_t mode,
    const float deltaSeconds) noexcept {
    ObserveCharacterControlUpdate(control);
    const auto original = reinterpret_cast<CharacterControlUpdateFn>(
        gCharacterControlTarget.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(control, mode, deltaSeconds);
    }
}

void __fastcall ShootingManagerTickDetour(void* manager) noexcept {
    ObserveShootingManagerTick(manager);
    const auto original = reinterpret_cast<ShootingManagerTickFn>(
        gManagerTickTarget.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(manager);
    }
}

} // namespace

bool InstallExactBuildObservationHooks(
    const std::uintptr_t moduleBase,
    const bool exactExecutableHash,
    HookManager& hooks,
    std::string& error) {
    if (gObservationHooksInstalled.load(std::memory_order_acquire)) {
        return true;
    }
    if (!exactExecutableHash || moduleBase == 0) {
        error = "Character-action diagnostic call-site hooks require the exact certified AI.exe SHA-256";
        return false;
    }

    const auto characterUpdate = moduleBase + kCharacterControlUpdateRva;
    const auto controlLoop = moduleBase + kCharacterControlLoopRva;
    const auto managerTick = moduleBase + kShootingManagerTickRva;
    const auto characterCalls = FindDirectCalls(
        controlLoop,
        kCharacterControlLoopBytes,
        characterUpdate);
    const auto managerCalls = FindDirectCalls(
        characterUpdate,
        kCharacterControlUpdateBytes,
        managerTick);
    if (characterCalls.size() != 3U || managerCalls.size() != 1U) {
        std::ostringstream stream;
        stream << "Certified call-site discovery mismatch (FUN_007342A0 calls="
               << characterCalls.size() << ", FUN_00473570 calls="
               << managerCalls.size() << ')';
        error = stream.str();
        return false;
    }

    const auto baseline = hooks.InstalledCount();
    const auto rollback = [&]() {
        std::string rollbackError;
        hooks.UninstallFrom(baseline, rollbackError);
        if (!rollbackError.empty()) {
            error += "; rollback: " + rollbackError;
        }
        gCharacterControlTarget.store(nullptr, std::memory_order_release);
        gManagerTickTarget.store(nullptr, std::memory_order_release);
        return false;
    };

    gCharacterControlTarget.store(
        reinterpret_cast<void*>(characterUpdate),
        std::memory_order_release);
    gManagerTickTarget.store(
        reinterpret_cast<void*>(managerTick),
        std::memory_order_release);

    for (std::size_t index = 0; index < characterCalls.size(); ++index) {
        CallRedirectRequest request{};
        request.name = "character_action_trace_control_" + std::to_string(index);
        request.callSite = characterCalls[index];
        request.expectedTarget = reinterpret_cast<void*>(characterUpdate);
        request.detour = reinterpret_cast<void*>(&CharacterControlUpdateDetour);
        if (!hooks.InstallCallRedirect(request, error)) {
            return rollback();
        }
    }

    CallRedirectRequest managerRequest{};
    managerRequest.name = "character_action_trace_shooting_manager";
    managerRequest.callSite = managerCalls.front();
    managerRequest.expectedTarget = reinterpret_cast<void*>(managerTick);
    managerRequest.detour = reinterpret_cast<void*>(&ShootingManagerTickDetour);
    if (!hooks.InstallCallRedirect(managerRequest, error)) {
        return rollback();
    }

    gObservationHooksInstalled.store(true, std::memory_order_release);
    return true;
}

bool ObservationHooksInstalled() noexcept {
    return gObservationHooksInstalled.load(std::memory_order_acquire);
}

bool InspectNpcTriggerShootRequestTables(
    const std::uintptr_t moduleBase,
    const bool exactExecutableHash,
    Diagnostics& diagnostics,
    std::string& message) {
    if (!exactExecutableHash) {
        message = "NPC_TriggerShootRequest table inspection requires the exact certified AI.exe SHA-256.";
        return false;
    }

    LoadedImageLayout layout{};
    std::string layoutError;
    if (!ReadLoadedImageLayout(moduleBase, layout, layoutError)) {
        message = "NPC_TriggerShootRequest runtime inspection failed: " + layoutError;
        return false;
    }

    const auto interfaceTable = moduleBase +
        kNpcTriggerShootRequestInterfaceTableRva;
    const auto implementationTable = moduleBase +
        kNpcTriggerShootRequestImplementationTableRva;
    if (!InRange(interfaceTable, layout.imageStart, layout.imageEnd) ||
        !InRange(implementationTable, layout.imageStart, layout.imageEnd)) {
        message = "NPC_TriggerShootRequest table RVAs fall outside the loaded image.";
        return false;
    }

    const auto interfaceSnapshot = InspectRuntimeTable(
        "interface_PTR_LAB_01293B18",
        kNpcTriggerShootRequestInterfaceTableRva,
        layout,
        diagnostics);
    const auto implementationSnapshot = InspectRuntimeTable(
        "implementation_PTR_FUN_01293EB0",
        kNpcTriggerShootRequestImplementationTableRva,
        layout,
        diagnostics);

    diagnostics.Log(
        "info",
        "npc_trigger_shoot_request_runtime_inspection_complete",
        "Exact-build read-only NPC_TriggerShootRequest runtime table inspection completed. No table entry was called and no game memory was modified.",
        {{"moduleBase", Diagnostics::Pointer(reinterpret_cast<const void*>(moduleBase))},
         {"imageSize", Diagnostics::Unsigned(layout.sizeOfImage), true},
         {"textStart", Diagnostics::Pointer(reinterpret_cast<const void*>(layout.textStart))},
         {"textEnd", Diagnostics::Pointer(reinterpret_cast<const void*>(layout.textEnd))},
         {"interfaceTableRva", HexValue(kNpcTriggerShootRequestInterfaceTableRva)},
         {"interfaceReadableEntries", Diagnostics::Unsigned(interfaceSnapshot.readableEntries), true},
         {"interfaceTextCandidates", Diagnostics::Unsigned(interfaceSnapshot.textCandidates), true},
         {"implementationTableRva", HexValue(kNpcTriggerShootRequestImplementationTableRva)},
         {"implementationReadableEntries", Diagnostics::Unsigned(implementationSnapshot.readableEntries), true},
         {"implementationTextCandidates", Diagnostics::Unsigned(implementationSnapshot.textCandidates), true},
         {"codeWindowBytes", Diagnostics::Unsigned(kNpcTriggerShootRequestCodeWindowBytes), true},
         {"readOnly", Diagnostics::Boolean(true), true}});

    std::ostringstream stream;
    stream << "Read-only NPC_TriggerShootRequest inspection completed: interface text candidates="
           << interfaceSnapshot.textCandidates
           << ", implementation text candidates="
           << implementationSnapshot.textCandidates
           << ". See npc_trigger_shoot_request_table, npc_trigger_shoot_request_candidate, and npc_trigger_shoot_request_runtime_inspection_complete in the native JSONL diagnostics. No table entry was invoked and no game memory was written.";
    message = stream.str();
    return true;
}

bool CompareCathodeActionNodeVTables(
    const std::uintptr_t moduleBase,
    const bool exactExecutableHash,
    Diagnostics& diagnostics,
    std::string& message) {
    if (!exactExecutableHash) {
        message =
            "Aim/Shoot/Reload vtable comparison requires the exact certified AI.exe SHA-256.";
        return false;
    }

    LoadedImageLayout layout{};
    std::string layoutError;
    if (!ReadLoadedImageLayout(moduleBase, layout, layoutError)) {
        message = "CATHODE action-node vtable comparison failed: " + layoutError;
        return false;
    }

    constexpr std::array<std::uintptr_t, 6U> kTables{
        kNpcTriggerAimRequestInterfaceTableRva,
        kNpcTriggerAimRequestImplementationTableRva,
        kNpcTriggerShootRequestInterfaceTableRva,
        kNpcTriggerShootRequestImplementationTableRva,
        kForceReloadWeaponInterfaceTableRva,
        kForceReloadWeaponImplementationTableRva,
    };
    for (const auto rva : kTables) {
        const auto address = moduleBase + rva;
        if (!InRange(address, layout.imageStart, layout.imageEnd)) {
            message =
                "One or more certified CATHODE action-node table RVAs fall outside the loaded image.";
            return false;
        }
    }

    const auto interfaceSummary = CompareActionNodeRole(
        "interface",
        kNpcTriggerAimRequestInterfaceTableRva,
        kNpcTriggerShootRequestInterfaceTableRva,
        kForceReloadWeaponInterfaceTableRva,
        layout,
        diagnostics);
    const auto implementationSummary = CompareActionNodeRole(
        "implementation",
        kNpcTriggerAimRequestImplementationTableRva,
        kNpcTriggerShootRequestImplementationTableRva,
        kForceReloadWeaponImplementationTableRva,
        layout,
        diagnostics);

    const auto totalAnchors = interfaceSummary.reloadAnchorCandidates +
        implementationSummary.reloadAnchorCandidates;
    diagnostics.Log(
        "info",
        "cathode_action_node_vtable_compare_complete",
        "Exact-build read-only Aim/Shoot/Reload vtable comparison completed. No table entry was called and no game memory was modified.",
        {{"moduleBase", Diagnostics::Pointer(reinterpret_cast<const void*>(moduleBase))},
         {"textStart", Diagnostics::Pointer(reinterpret_cast<const void*>(layout.textStart))},
         {"textEnd", Diagnostics::Pointer(reinterpret_cast<const void*>(layout.textEnd))},
         {"aimInterfaceTableRva", HexValue(kNpcTriggerAimRequestInterfaceTableRva)},
         {"shootInterfaceTableRva", HexValue(kNpcTriggerShootRequestInterfaceTableRva)},
         {"reloadInterfaceTableRva", HexValue(kForceReloadWeaponInterfaceTableRva)},
         {"aimImplementationTableRva", HexValue(kNpcTriggerAimRequestImplementationTableRva)},
         {"shootImplementationTableRva", HexValue(kNpcTriggerShootRequestImplementationTableRva)},
         {"reloadImplementationTableRva", HexValue(kForceReloadWeaponImplementationTableRva)},
         {"knownReloadExecutorRva", HexValue(kForceReloadWeaponExecutorRva)},
         {"interfaceReloadAnchorCandidates", Diagnostics::Unsigned(interfaceSummary.reloadAnchorCandidates), true},
         {"implementationReloadAnchorCandidates", Diagnostics::Unsigned(implementationSummary.reloadAnchorCandidates), true},
         {"totalReloadAnchorCandidates", Diagnostics::Unsigned(totalAnchors), true},
         {"readOnly", Diagnostics::Boolean(true), true}});

    std::ostringstream stream;
    stream << "Read-only Aim/Shoot/Reload vtable comparison completed. Reload execution-anchor candidates="
           << totalAnchors
           << ". See cathode_action_node_vtable_slot, "
              "cathode_action_node_execution_slot_candidate, and "
              "cathode_action_node_vtable_compare_complete in native JSONL diagnostics. "
              "No table entry was invoked and no game memory was written.";
    message = stream.str();
    return true;
}

bool CanArmTrace(std::string& reason) {
    std::lock_guard lock(gMutex);
    if (gTrace.shutdown) {
        reason = "character-action diagnostics are shutting down";
        return false;
    }
    if (gTrace.active) {
        reason = "a bounded ShootingRequest trace is already active";
        return false;
    }
    if (gTrace.retainedForCancel) {
        reason = "the previous traced ShootingRequest is still retained for explicit cancellation";
        return false;
    }
    reason.clear();
    return true;
}

bool ArmTrace(ShootTraceArm arm, std::string& error) {
    std::lock_guard lock(gMutex);
    if (gTrace.shutdown) {
        error = "character-action diagnostics are shutting down";
        return false;
    }
    if (gTrace.active || gTrace.retainedForCancel) {
        error = gTrace.active
            ? "a bounded ShootingRequest trace is already active"
            : "the previous probe request is still retained for explicit cancellation";
        return false;
    }
    if (arm.characterAddress < 0x10000U ||
        arm.shootingManager < 0x10000U ||
        arm.equipmentController < 0x10000U ||
        arm.request.object == nullptr ||
        arm.request.control == nullptr ||
        arm.releaseAddress == nullptr ||
        arm.cancelAddress == nullptr ||
        arm.diagnostics == nullptr) {
        error = "invalid bounded ShootingRequest trace arguments";
        return false;
    }

    gTrace = {};
    gTrace.active = true;
    gTrace.character = arm.characterAddress;
    gTrace.control = arm.characterAddress + kCharacterControlOffset;
    gTrace.manager = arm.shootingManager;
    gTrace.equipment = arm.equipmentController;
    gTrace.generation = arm.generation;
    gTrace.startedTick = GetTickCount64();
    gTrace.request = arm.request;
    gTrace.releaseAddress = arm.releaseAddress;
    gTrace.cancelAddress = arm.cancelAddress;
    gTrace.diagnostics = arm.diagnostics;

    gCharacterControlCalls.store(0, std::memory_order_release);
    gManagerTickCalls.store(0, std::memory_order_release);
    gObservedControl.store(gTrace.control, std::memory_order_release);
    gObservedManager.store(gTrace.manager, std::memory_order_release);

    gTrace.first = ReadSample(gTrace);
    gTrace.last = gTrace.first;
    gTrace.hasFirst = gTrace.first.readable;
    return true;
}

void ObserveEngineFrame(const std::uint64_t currentGeneration) noexcept {
    std::lock_guard lock(gMutex);
    if (gTrace.shutdown) {
        return;
    }
    if (!gTrace.active && gTrace.retainedForCancel &&
        currentGeneration != gTrace.generation) {
        ReleaseHeldReference(gTrace);
        gTrace.retainedForCancel = false;
        gTrace.diagnostics = nullptr;
        return;
    }
    if (!gTrace.active) {
        return;
    }
    if (currentGeneration != gTrace.generation) {
        gTrace.last = ReadSample(gTrace);
        FinalizeTraceLocked(gTrace, "level_generation_changed");
        if (gTrace.retainedForCancel) {
            // The Character lifecycle changed. Do not call request cancellation
            // against a manager from the previous generation; just release our
            // strong reference on the engine thread.
            ReleaseHeldReference(gTrace);
            gTrace.retainedForCancel = false;
        }
        return;
    }

    const auto sample = ReadSample(gTrace);
    if (!gTrace.hasFirst && sample.readable) {
        gTrace.first = sample;
        gTrace.hasFirst = true;
    }
    if (gTrace.last.readable && sample.readable) {
        if (sample.currentState != gTrace.last.currentState) {
            ++gTrace.currentStateTransitions;
        }
        if (sample.requestedState != gTrace.last.requestedState) {
            ++gTrace.requestedStateTransitions;
        }
        if (sample.requestState != gTrace.last.requestState) {
            ++gTrace.requestStateTransitions;
        }
        if (sample.requestBudget != gTrace.last.requestBudget) {
            ++gTrace.requestBudgetTransitions;
        }
        if (sample.primaryA2 != gTrace.last.primaryA2) {
            ++gTrace.primaryTransitions;
        }
    }
    gTrace.last = sample;
    ++gTrace.frames;
    if (sample.currentState == 3) {
        ++gTrace.state3Frames;
        gTrace.everCurrent3 = true;
    }
    if (sample.currentState == 5) {
        ++gTrace.state5Frames;
        gTrace.everCurrent5 = true;
    }
    gTrace.everRequested3 = gTrace.everRequested3 || sample.requestedState == 3;
    gTrace.everRequested5 = gTrace.everRequested5 || sample.requestedState == 5;
    gTrace.everPrimary = gTrace.everPrimary || sample.primaryA2 != 0;

    const auto elapsed = GetTickCount64() - gTrace.startedTick;
    if (!sample.readable) {
        FinalizeTraceLocked(gTrace, "memory_unreadable");
        return;
    }
    if (sample.activeCount == 0 &&
        sample.requestState >= 3 &&
        sample.requestState <= 6) {
        FinalizeTraceLocked(gTrace, "request_left_active_queue");
        return;
    }
    if (gTrace.frames >= kTraceMaximumFrames ||
        elapsed >= kTraceMaximumMilliseconds) {
        FinalizeTraceLocked(gTrace, "bounded_window_elapsed");
    }
}

void ObserveCharacterControlUpdate(void* control) noexcept {
    if (reinterpret_cast<std::uintptr_t>(control) ==
        gObservedControl.load(std::memory_order_acquire)) {
        gCharacterControlCalls.fetch_add(1, std::memory_order_acq_rel);
    }
}

void ObserveShootingManagerTick(void* manager) noexcept {
    if (reinterpret_cast<std::uintptr_t>(manager) ==
        gObservedManager.load(std::memory_order_acquire)) {
        gManagerTickCalls.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool CancelHeldRequest(const std::uintptr_t characterAddress, std::string& message) {
    std::lock_guard lock(gMutex);
    if (gTrace.active) {
        message = "The diagnostic window is still active. Wait for character_action_shoot_trace_complete before cancelling.";
        return false;
    }
    if (gTrace.retainedForCancel && gTrace.character != characterAddress) {
        message = "The retained ShootingRequest belongs to a different Character instance.";
        return false;
    }
    if (!gTrace.retainedForCancel ||
        gTrace.request.object == nullptr ||
        gTrace.request.control == nullptr ||
        gTrace.cancelAddress == nullptr) {
        message = "No retained ShootingRequest is available for cancellation.";
        return false;
    }

    const bool cancelled = CancelRequestProtected(
        gTrace.cancelAddress,
        gTrace.request.object);
    if (!cancelled) {
        message = "FUN_00471E90 raised a protected fault while cancelling the retained probe request.";
        return false;
    }

    ReleaseHeldReference(gTrace);
    gTrace.retainedForCancel = false;
    message = "The retained probe ShootingRequest was cancelled through FUN_00471E90 and the diagnostic shared-ref was released.";
    if (gTrace.diagnostics != nullptr) {
        gTrace.diagnostics->Log(
            "info",
            "character_action_shoot_trace_cancelled",
            message,
            {{"classification", gTrace.finalClassification}});
    }
    return true;
}

bool RetireOwnedRequestForActionRuntime(const std::uintptr_t characterAddress,
    const std::uint64_t generation, std::string& error) {
    std::lock_guard lock(gMutex);
    const bool ownsReference = gTrace.request.object != nullptr &&
        gTrace.request.control != nullptr && gTrace.character == characterAddress;
    if (ownsReference) {
        std::uintptr_t manager{}, equipment{}, owner{}, requestManager{};
        std::int32_t requestType{};
        const auto request = reinterpret_cast<std::uintptr_t>(gTrace.request.object);
        // A retained strong ref proves ownership of R, not the lifetime of M/C.
        // Do not follow an obsolete manager after a level/Character replacement.
        if (gTrace.generation != generation ||
            !ReadAt(characterAddress, 0x1100U, equipment) || equipment != gTrace.equipment ||
            !ReadAt(equipment, 0x1528U, owner) || owner != characterAddress ||
            !ReadAt(characterAddress, 0x34C0U, manager) || manager != gTrace.manager ||
            !ReadAt(request, 0x14U, requestManager) || requestManager != manager ||
            !ReadAt(request, 0x10U, requestType) || requestType != 1 ||
            gTrace.cancelAddress == nullptr || gTrace.releaseAddress == nullptr) {
            error = "The retained legacy AIMP request no longer matches this live instance.";
            return false;
        }
        gTrace.active = false;
        gObservedControl.store(0U, std::memory_order_release);
        gObservedManager.store(0U, std::memory_order_release);
        if (!CancelRequestProtected(gTrace.cancelAddress, gTrace.request.object)) {
            error = "Native cancellation of the owned legacy request failed.";
            return false; // retain the reference, never retry with a guessed pointer
        }
        ReleaseHeldReference(gTrace);
        gTrace.retainedForCancel = false;
        gTrace.complete = true;
        gTrace.diagnostics = nullptr;
    }
    // Admission-only coexistence check. The functional runtime never reads or
    // mutates the ShootingManager after this migration boundary.
    std::uintptr_t manager{};
    std::int32_t activeCount{};
    if (!ReadAt(characterAddress, 0x34C0U, manager)) {
        error = "Cannot validate the legacy request boundary.";
        return false;
    }
    if (manager != 0U && (!ReadAt(manager, 0xCU, activeCount) || activeCount != 0)) {
        error = "Another active ShootingRequest remains; it does not belong to this runtime.";
        return false;
    }
    return true;
}

void BeginLogicalShutdown() noexcept {
    std::lock_guard lock(gMutex);
    gTrace.shutdown = true;
    gTrace.active = false;
    gTrace.diagnostics = nullptr;
    gObservedControl.store(0, std::memory_order_release);
    gObservedManager.store(0, std::memory_order_release);
    // Do not invoke native release/cancel during bootstrap shutdown because the
    // shutdown thread is not guaranteed to be the engine thread. Process exit
    // owns any remaining reference.
}

} // namespace aimp::character_action_probe
