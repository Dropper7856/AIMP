#include "aimp/CathodeEquipmentGive.h"
#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/SignatureResolver.h"

#include <Windows.h>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace aimp::cathode_equipment {
namespace {
static_assert(sizeof(void*) == 4, "Retail CATHODE requires the MSVC x86 ABI.");

// Preferred VAs from the supplied AI.exe (SHA256 00d6...8364), NOT
// Cheat Engine's .text-relative offsets. All calls are relocated through base.
template <typename T> T Fn(std::uintptr_t base, std::uintptr_t va) noexcept {
    return reinterpret_cast<T>(base + va - 0x00400000U);
}
template <typename T> T Read(std::uintptr_t address) noexcept {
    return crash_guard::SafeRead<T>(reinterpret_cast<const void*>(address)).value_or(T{});
}
std::uintptr_t Payload(void* ref) noexcept {
    return ref == nullptr ? 0U : Read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(ref) + 0x0CU);
}
std::uintptr_t Virtual(std::uintptr_t object, std::uintptr_t slot) noexcept {
    const auto table = Read<std::uintptr_t>(object);
    return table == 0U ? 0U : Read<std::uintptr_t>(table + slot);
}
using Factory = void**(__cdecl*)(void**, void**, const std::uint32_t*);
using Assign = void*(__thiscall*)(void*, void**);
using FindComposite = std::uint32_t*(__thiscall*)(void*, std::uint32_t*, const char*);
using Create = void**(__thiscall*)(void*, void**, const std::uint32_t*, const float*);
using Finalize = std::uint8_t(__thiscall*)(void*, void*); // consumes ONE retain
using SetKind = void(__thiscall*)(void*, std::uint8_t);
using Hash = std::uint32_t*(__thiscall*)(void*, const char*);
using Interface = void*(__thiscall*)(void*);
using RuntimeHolder = void**(__thiscall*)(void*, void**);
using PublicInput = void(__thiscall*)(void*, void**, const std::uint32_t*, std::uint32_t);
using Query = std::uint8_t(__thiscall*)(void*, void**, const std::uint32_t*, void**);
using Typed = void*(__cdecl*)(void**);
using WeakAssign = void(__thiscall*)(void*, void*);
using PrepareBindings = void(__thiscall*)(void*, void**);
using Pin = void*(__thiscall*)(void*, void**);
using Unpin = void(__fastcall*)(void*);
using Bind = std::uint8_t(__stdcall*)(void*, void**, const std::uint32_t*, const std::uint32_t*, double);
using BoolFactory = void**(__thiscall*)(void*, void**, void**, const std::uint32_t*, const std::uint8_t*);
using GetFlag = std::uint8_t(__thiscall*)(void*, void**);
using GetInput = void**(__thiscall*)(void*, void**, void**);
using BuildWork = void**(__thiscall*)(void*, void**, void**, std::uint32_t, std::uint32_t, void*);
using EnterCritical = void(__fastcall*)(void*);
using LeaveCritical = void(__cdecl*)();
using ReleaseWork = void(__fastcall*)(void*);
using Execute = std::uint8_t(__thiscall*)(void*, void**, void**);
using EquipmentItem = void*(__thiscall*)(void*);
using InitializeNpcResources = std::uint32_t(__thiscall*)(void*, std::int32_t);

void Release(std::uintptr_t base, void*& ref) {
    if (ref == nullptr) return;
    void* empty = nullptr;
    Fn<Assign>(base, 0x004673C0U)(&ref, &empty);
}
std::uint32_t Guid(std::uintptr_t base, const char* name) {
    std::uint32_t value{};
    Fn<Hash>(base, 0x008BF7F0U)(&value, name);
    return value;
}
void* GetInterface(void* ref) {
    const auto entity = Payload(ref);
    const auto entry = Virtual(entity, 0x6CU);
    return entry == 0U ? nullptr : reinterpret_cast<Interface>(entry)(reinterpret_cast<void*>(entity));
}
Result Fail(Record& r, Result result) noexcept {
    r.stage = Stage::Failed;
    r.result = result;
    return result;
}
// A public equipment SPAWN/Give attaches the asset but is not a promise of
// a full magazine. AIMP's manual Give policy supplies ONE native capacity.
// The native routine also updates the associated E4 accounting and CATHODE
// notifications. It is never called from Primary START or an equipment tick.
// Source: AI.exe.c FUN_007AD7E0, 735235-735280. Do not call a reload animation.
Result InitializeGivenResources(std::uintptr_t base, std::uintptr_t character,
    std::uintptr_t equipmentManager, Record& r) {
    if (r.initialResourcesCompleted) return Result::Equipped;
    if (r.initialResourcesAttempted)
        return Fail(r, Result::ResourceInitializationFailed); // no partial-call retry
    if (++r.resourcePolls > 64U) return Fail(r, Result::TimedOut);
    r.operation = "wait_for_initial_equipment_resources";

    const auto wrapper = reinterpret_cast<std::uintptr_t>(r.resolvedEquipment);
    const auto getter = Virtual(wrapper, 0x14U);
    const auto& profile = Retail2015Profile();
    const auto codeBegin = base + profile.textRva;
    const auto codeEnd = codeBegin + profile.textSize;
    if (getter < codeBegin || getter >= codeEnd)
        return Fail(r, Result::OutputMismatch);
    auto* item = reinterpret_cast<EquipmentItem>(getter)(r.resolvedEquipment);
    if (item == nullptr) {
        // Equipment with no common weapon item (e.g. a tool) has no magazine
        // contract here. Do not guess a layout from its name or its slot.
        r.initialResourcesCompleted = true;
        r.initialResourcePolicy = "no_common_resource_item";
        return Result::Equipped;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(item);
    if (address < 0x10000U || address > 0xFFFEFE00U)
        return Fail(r, Result::ResourceInitializationFailed);
    const auto owner = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(address + 0x28U));
    const auto ownerId = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(address + 0x2CU));
    if (!owner.has_value() || !ownerId.has_value())
        return Fail(r, Result::ResourceInitializationFailed);
    if (*owner == 0U) return Result::Pending;
    if (*owner != character || *ownerId != r.runtimeId ||
        Read<std::uint32_t>(character + 0x50U) != r.runtimeId ||
        Read<std::uintptr_t>(character + 0x1100U) != equipmentManager ||
        Read<std::uintptr_t>(equipmentManager + 0x1528U) != character ||
        Read<std::uintptr_t>(equipmentManager + r.expectedSlot * 4U) != wrapper)
        return Fail(r, Result::InvalidCharacter);

    const auto descriptor = Read<std::uintptr_t>(character + 0x64U);
    if (descriptor == 0U || Read<std::int32_t>(descriptor + 4U) != 3 ||
        Read<std::uint8_t>(descriptor + 0x1CU) != 0U)
        return Fail(r, Result::InvalidCharacter);
    if (Read<std::uint8_t>(descriptor + 0x1FU) == 0U ||
        Read<std::uint8_t>(base + 0x012F0C8CU) == 0U)
        return Result::Pending;

    const auto capacity = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(address + 0xECU));
    const auto resources = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(address + 0xE0U));
    const auto ammoKind = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(address + 0xD0U));
    if (!capacity.has_value() || !resources.has_value() || !ammoKind.has_value() ||
        *capacity < 0 || *capacity > 1000000 || *resources < 0 || *resources > 1000000)
        return Fail(r, Result::ResourceInitializationFailed);
    if (*ammoKind < 0 && *capacity > 0) return Result::Pending;
    r.nativeResourceCapacity = *capacity;
    r.initialResourcesBefore = *resources;
    r.initialResourcesAfter = *resources;
    if (*capacity == 0 || *resources >= *capacity) {
        r.initialResourcesCompleted = true;
        r.initialResourcePolicy = *capacity == 0 ? "no_magazine_capacity" : "already_full";
        return Result::Equipped;
    }

    // Never refill an active cycle. The manual Give must finish before Aim or
    // Primary are used. Waiting is bounded, with no direct state/flag writes.
    std::uint8_t flags[8]{};
    if (!crash_guard::TryCopy(flags,
            reinterpret_cast<const void*>(equipmentManager + 0xA0U), sizeof(flags)))
        return Fail(r, Result::ResourceInitializationFailed);
    for (const auto flag : flags) if (flag != 0U) return Result::Pending;
    if (Read<std::int32_t>(address + 0xD8U) == 1) return Result::Pending;

    r.operation = "initialize_native_equipment_resources";
    r.initialResourcesAttempted = true; // latch BEFORE any engine callback
    (void)Fn<InitializeNpcResources>(base, 0x007AD7E0U)(item, *ammoKind);
    // The native routine can emit callbacks. Revalidate the owner/instance
    // before observing the result; never assume a readable pointer is enough.
    if (Read<std::uint32_t>(character + 0x50U) != r.runtimeId ||
        Read<std::uintptr_t>(character + 0x1100U) != equipmentManager ||
        Read<std::uintptr_t>(equipmentManager + 0x1528U) != character ||
        Read<std::uintptr_t>(equipmentManager + r.expectedSlot * 4U) != wrapper ||
        Read<std::uintptr_t>(address + 0x28U) != character ||
        Read<std::uint32_t>(address + 0x2CU) != r.runtimeId)
        return Fail(r, Result::InvalidCharacter);
    const auto after = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(address + 0xE0U));
    const auto accounting = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(address + 0xE4U));
    if (!after.has_value() || !accounting.has_value() ||
        *after != *capacity || *accounting != *capacity)
        return Fail(r, Result::ResourceInitializationFailed);
    r.initialResourcesAfter = *after;
    r.initialResourcesCompleted = true;
    r.initialResourcePolicy = "native_capacity_once";
    return Result::Equipped;
}

Result StepNative(std::uintptr_t base, std::uintptr_t character, Record& r) {
    // CathodeLib's retail EQUIPMENT_SLOT enum defines 0..10, with -1 as
    // UNKNOWN. The Launcher reads the selected leaf node's equipment_slot
    // enum directly from COMMANDS and sends that value with the catalog.
    constexpr std::uint32_t kMaximumRetailEquipmentSlot = 10U;

    if (r.stage == Stage::Failed) return r.result;
    if (character < 0x10000U || (character & 0x0FU) != 0U ||
        Read<std::uint32_t>(character + 0x50U) != r.runtimeId)
        return Fail(r, Result::InvalidCharacter);

    auto* manager = Read<void*>(base + 0x0134EF40U);
    if (manager == nullptr) return Result::Pending;
    const auto equipmentManager = Read<std::uintptr_t>(character + 0x1100U);
    if (equipmentManager == 0U) return Result::Pending;

    // Success is defined by the exact EquipmentInstance returned from the
    // selected public output appearing in the Character's native slot table.
    // This avoids assuming +0x28/+0x2C/+0x48 on every equipment subclass.
    if (r.resolvedEquipment != nullptr) {
        if (r.expectedSlot > kMaximumRetailEquipmentSlot)
            return Fail(r, Result::InvalidCatalog);
        const auto attached = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(
                equipmentManager + r.expectedSlot * sizeof(std::uint32_t)));
        if (!attached.has_value())
            return Result::Pending;
        if (*attached == reinterpret_cast<std::uintptr_t>(r.resolvedEquipment)) {
            r.equipmentSlot = r.expectedSlot;
            const auto resources = InitializeGivenResources(base, character, equipmentManager, r);
            if (resources != Result::Equipped) return resources;
            r.stage = Stage::Equipped;
            r.result = Result::Equipped;
            r.operation = "equipment_attached";
            return Result::Equipped;
        }
    }

    if (++r.polls > 64U) return Fail(r, Result::TimedOut);
    if (r.stage == Stage::GiveDispatched) return Result::Pending;

    if (r.stage == Stage::Empty) {
        r.operation = "validate_catalog";
        if (r.compositeName.empty() || r.compositeName.size() > 255U ||
            r.equipmentEntityId == 0U || r.outputId == 0U || r.targetInputId == 0U ||
            r.expectedSlot > kMaximumRetailEquipmentSlot ||
            r.spawnId != Guid(base, "SPAWN"))
            return Fail(r, Result::InvalidCatalog);

        r.operation = "create_equipment_composite";
        r.compositeId = 0U;
        Fn<FindComposite>(base, 0x00922520U)(
            manager, &r.compositeId, r.compositeName.c_str());
        if (r.compositeId == 0U) {
            const auto separator = r.compositeName.find_last_of("\\/");
            if (separator != std::string::npos && separator + 1U < r.compositeName.size()) {
                const auto tail = r.compositeName.substr(separator + 1U);
                Fn<FindComposite>(base, 0x00922520U)(
                    manager, &r.compositeId, tail.c_str());
            }
        }
        if (r.compositeId == 0U) return Fail(r, Result::CompositeMissing);

        float xyz[3]{};
        if (!crash_guard::TryCopy(
                xyz, reinterpret_cast<void*>(character + 0x70U), sizeof(xyz)) ||
            !std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]))
            return Fail(r, Result::InvalidCharacter);
        const float transform[12]{
            1, 0, 0, xyz[0],
            0, 1, 0, xyz[1],
            0, 0, 1, xyz[2]};
        Fn<Create>(base, 0x00930950U)(manager, &r.root, &r.compositeId, transform);
        const auto root = Payload(r.root);
        if (root == 0U) return Fail(r, Result::CreateFailed);
        Fn<SetKind>(base, 0x00A2A900U)(reinterpret_cast<void*>(root), 6U);
        const auto flags = static_cast<std::uint8_t>(
            Read<std::uint8_t>(root + 0x2DU) | 4U);
        if (!crash_guard::TryWrite(
                reinterpret_cast<void*>(root + 0x2DU), &flags, sizeof(flags)))
            return Fail(r, Result::CreateFailed);

        r.operation = "finalize_equipment_composite";
        void* consumed = nullptr;
        Fn<Assign>(base, 0x004673C0U)(&consumed, &r.root);
        if (Fn<Finalize>(base, 0x00924B60U)(manager, consumed) == 0U)
            return Fail(r, Result::FinalizeFailed);

        r.operation = "equipment_public_spawn";
        const auto dispatch = Virtual(root, 0x10CU);
        if (dispatch != base + 0x005425A0U) return Fail(r, Result::CreateFailed);
        r.stage = Stage::WaitingOutput;
        reinterpret_cast<PublicInput>(dispatch)(
            reinterpret_cast<void*>(root), &r.root, &r.spawnId, 0U);
        return Result::Pending;
    }

    r.operation = "resolve_runtime_composite";
    if (r.composite == nullptr) {
        const auto rootPayload = Payload(r.root);
        const auto getRuntime = Virtual(rootPayload, 0x118U);
        if (getRuntime != base + 0x0053F820U)
            return Fail(r, Result::OutputMismatch);
        auto** holder = reinterpret_cast<RuntimeHolder>(getRuntime)(
            reinterpret_cast<void*>(rootPayload), &r.root);
        if (holder == nullptr ||
            Read<void*>(reinterpret_cast<std::uintptr_t>(holder)) == nullptr)
            return Result::Pending;
        Fn<Assign>(base, 0x004673C0U)(&r.composite, holder);
    }

    auto* compositeInterface = GetInterface(r.composite);
    if (compositeInterface == nullptr) return Fail(r, Result::OutputMismatch);
    r.operation = "resolve_equipment_output";
    Release(base, r.weapon);
    r.outputQuerySucceeded = Fn<Query>(base, 0x008F8B50U)(
        compositeInterface, &r.composite, &r.outputId, &r.weapon) != 0U;
    if (r.weapon == nullptr) {
        r.operation = r.outputQuerySucceeded
            ? "equipment_reference_null"
            : "equipment_output_unresolved";
        return Result::Pending;
    }

    const auto weaponPayload = Payload(r.weapon);
    if (weaponPayload == 0U ||
        Read<std::uint32_t>(weaponPayload + 0x0CU) != r.equipmentEntityId)
        return Fail(r, Result::OutputMismatch);

    r.operation = "resolve_equipment_instance";
    r.resolvedEquipment = Fn<Typed>(base, 0x008B4B80U)(&r.weapon);
    if (r.resolvedEquipment == nullptr) return Result::Pending;
    r.operation = "create_character_reference";
    void* noParent = nullptr;
    const std::uint32_t characterDescriptor[]{
        Guid(base, "AIMP_Equipment_Character"), Guid(base, "Character")};
    Fn<Factory>(base, 0x004C2A00U)(
        &r.characterNode, &noParent, characterDescriptor);
    const auto characterNode = Payload(r.characterNode);
    if (characterNode == 0U ||
        Read<std::uintptr_t>(characterNode) != base + 0x00DD08D8U)
        return Fail(r, Result::CharacterBridgeFailed);
    (void)Fn<Typed>(base, 0x008B4760U)(&r.characterNode);
    Fn<WeakAssign>(base, 0x0040A1D0U)(
        reinterpret_cast<void*>(characterNode + 0xE0U),
        reinterpret_cast<void*>(character));
    r.resolvedCharacter = Fn<Typed>(base, 0x008B4760U)(&r.characterNode);
    if (r.resolvedCharacter != reinterpret_cast<void*>(character))
        return Fail(r, Result::CharacterBridgeFailed);

    r.operation = "create_give_to_character_node";
    const std::uint32_t giveDescriptor[]{
        Guid(base, "AIMP_Equipment_Give"), Guid(base, "WEAPON_GiveToCharacter")};
    Fn<Factory>(base, 0x00558500U)(&r.giveNode, &noParent, giveDescriptor);
    const auto givePayload = Payload(r.giveNode);
    auto* giveInterface = GetInterface(r.giveNode);
    if (givePayload == 0U || giveInterface == nullptr ||
        Virtual(reinterpret_cast<std::uintptr_t>(giveInterface), 0x200U) !=
            base + 0x00124690U)
        return Fail(r, Result::BindFailed);

    (void)Fn<Typed>(base, 0x008B4760U)(&r.giveNode);
    Fn<Pin>(base, 0x008BCA90U)(&r.bindingPin, &r.giveNode);
    Fn<PrepareBindings>(base, 0x008BD720U)(
        static_cast<std::byte*>(giveInterface) + 4U, &r.giveNode);
    const auto referenceKey = Read<std::uint32_t>(base + 0x0134D304U);
    const auto characterKey = Read<std::uint32_t>(base + 0x0135154CU);
    const auto weaponKey = Read<std::uint32_t>(base + 0x01351550U);
    if (referenceKey == 0U || characterKey != 0xC561798AU ||
        weaponKey != 0x40087171U)
        return Fail(r, Result::BindFailed);

    const auto flagKey = Read<std::uint32_t>(base + 0x01351554U);
    auto* allocator = Read<void*>(base + 0x0134E268U);
    const auto boolType = Read<std::uint32_t>(base + 0x0134F848U);
    if (flagKey == 0U || allocator == nullptr || boolType == 0U)
        return Fail(r, Result::BindFailed);
    const std::uint32_t flagDescriptor[]{
        Guid(base, "AIMP_Equipment_Flag"), boolType};
    const std::uint8_t falseValue = 0U;
    Fn<BoolFactory>(base, 0x009415F0U)(
        allocator, &r.flagNode, &noParent, flagDescriptor, &falseValue);
    if (r.flagNode == nullptr) return Fail(r, Result::BindFailed);

    r.operation = "bind_give_to_character_inputs";
    if (Fn<Bind>(base, 0x00964CF0U)(
            giveInterface, &r.characterNode, &characterKey, &referenceKey, 0.0) == 0U ||
        Fn<Bind>(base, 0x00964CF0U)(
            giveInterface, &r.weapon, &weaponKey, &referenceKey, 0.0) == 0U ||
        Fn<Bind>(base, 0x00964CF0U)(
            giveInterface, &r.flagNode, &flagKey, &referenceKey, 0.0) == 0U)
        return Fail(r, Result::BindFailed);

    r.operation = "preflight_give_to_character_inputs";
    void* characterInput = nullptr;
    void* weaponInput = nullptr;
    Fn<GetInput>(base, 0x008FABB0U)(
        giveInterface, &characterInput, &r.giveNode);
    Fn<GetInput>(base, 0x008FAC50U)(
        giveInterface, &weaponInput, &r.giveNode);
    const bool sameCharacter = characterInput != nullptr &&
        Fn<Typed>(base, 0x008B4760U)(&characterInput) ==
            reinterpret_cast<void*>(character);
    const bool sameWeapon = weaponInput == r.weapon &&
        Fn<Typed>(base, 0x008B4B80U)(&weaponInput) == r.resolvedEquipment;
    Release(base, characterInput);
    Release(base, weaponInput);
    const bool sameFlag =
        Fn<GetFlag>(base, 0x008E6150U)(giveInterface, &r.giveNode) == 0U;
    if (!sameCharacter || !sameWeapon || !sameFlag)
        return Fail(r, Result::PreflightFailed);

    r.operation = "execute_weapon_give_to_character";
    Fn<BuildWork>(base, 0x00926FF0U)(
        manager, &r.work, &r.giveNode, 0x40000000U, 0U, nullptr);
    if (r.work == nullptr) return Fail(r, Result::DispatchRejected);
    r.giveStarted = true;
    r.stage = Stage::GiveDispatched;
    const auto accepted = Fn<Execute>(base, 0x00936A00U)(
        reinterpret_cast<void*>(givePayload + 4U), &r.giveNode, &r.work);
    Fn<ReleaseWork>(base, 0x009252F0U)(r.work);
    r.work = nullptr;
    Fn<Unpin>(base, 0x008BCBB0U)(&r.bindingPin);
    if (accepted == 0U) return Fail(r, Result::DispatchRejected);

    r.operation = "wait_for_equipment_owner";
    return Result::Pending;
}
} // namespace

std::uintptr_t Validate(const SignatureResolver& resolver) noexcept {
    if (!resolver.ExactBuildValidated()) return 0U;
    const auto create = reinterpret_cast<std::uintptr_t>(resolver.Resolve(SymbolId::CathodeCreateEntity));
    if (create < 0x00530950U) return 0U;
    const auto base = create - 0x00530950U;
    const auto* resourceInit = resolver.Resolve(SymbolId::CharacterItemInitializeNpcResources);
    const auto* resourceSpec = Retail2015Profile().Find(SymbolId::CharacterItemInitializeNpcResources);
    if (resourceInit != reinterpret_cast<const void*>(base + 0x003AD7E0U) ||
        resourceSpec == nullptr || resourceSpec->expected.bytes.empty()) return 0U;
    // Resolve validates the nonempty build signature. Also pin the exact RVA:
    // never accept a scan result for this mutating boundary.
    // Exact file identity is checked upstream. Also reject modified prologues
    // at the new boundaries (prefixes do not include relocated immediates).
    struct Entry { std::uintptr_t va; std::uint16_t prefix; };
    constexpr Entry entries[]{
        {0x0093F820U, 0x418DU},
        {0x0040A1D0U, 0x448BU},
        {0x004673C0U, 0x8B55U},
        {0x004C2A00U, 0xFF6AU},
        {0x00524690U, 0xFF6AU},
        {0x00558500U, 0xFF6AU},
        {0x008B4760U, 0x8B55U},
        {0x008B4B80U, 0x8B55U},
        {0x008BCA90U, 0x8B55U},
        {0x008BCBB0U, 0x8B55U},
        {0x008BD720U, 0x8B55U},
        {0x008BF7F0U, 0x8B55U},
        {0x008E6150U, 0x8B55U},
        {0x008F8B50U, 0x8B55U},
        {0x008FABB0U, 0x8B55U},
        {0x008FAC50U, 0x8B55U},
        {0x0091E1B0U, 0x6856U},
        {0x0091E1D0U, 0x4868U},
        {0x00922520U, 0x8B55U},
        {0x00924B60U, 0x8B55U},
        {0x009252F0U, 0x8B56U},
        {0x00926FF0U, 0x8B55U},
        {0x00930950U, 0x8B55U},
        {0x00936A00U, 0x8B55U},
        {0x009415F0U, 0x8B55U},
        {0x009425A0U, 0x8B55U},
        {0x00964CF0U, 0x8B55U},
        {0x00A2A900U, 0x8B55U},
    };
    for (const auto& entry : entries)
        if (Read<std::uint16_t>(base + entry.va - 0x00400000U) != entry.prefix) return 0U;
    return base;
}

namespace {
void ReleaseOwned(std::uintptr_t base, Record& record) {
    if (record.bindingPin != nullptr)
        Fn<Unpin>(base, 0x008BCBB0U)(&record.bindingPin);
    if (record.work != nullptr) {
        Fn<ReleaseWork>(base, 0x009252F0U)(record.work);
        record.work = nullptr;
    }
    Release(base, record.giveNode);
    Release(base, record.characterNode);
    Release(base, record.flagNode);
    Release(base, record.weapon);
    const auto root = Payload(record.root);
    if (root != 0U) {
        const auto flags = static_cast<std::uint8_t>(Read<std::uint8_t>(root + 0x2DU) | 1U);
        (void)crash_guard::TryWrite(reinterpret_cast<void*>(root + 0x2DU), &flags, sizeof(flags));
    }
    Release(base, record.composite);
    Release(base, record.root);
}
} // namespace

bool IsAttached(
    const std::uintptr_t character,
    const Record& record) noexcept {
    constexpr std::uint32_t kMaximumRetailEquipmentSlot = 10U;
    if (character < 0x10000U || record.resolvedEquipment == nullptr ||
        record.expectedSlot > kMaximumRetailEquipmentSlot)
        return false;

    const auto runtimeId = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(character + 0x50U));
    const auto equipmentManager = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(character + 0x1100U));
    if (!runtimeId.has_value() || *runtimeId != record.runtimeId ||
        !equipmentManager.has_value() || *equipmentManager == 0U)
        return false;

    const auto attached = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(
            *equipmentManager + record.expectedSlot * sizeof(std::uint32_t)));
    return attached.has_value() &&
        *attached == reinterpret_cast<std::uintptr_t>(record.resolvedEquipment);
}

Result Step(std::uintptr_t base, std::uintptr_t character, Record& record) noexcept {
    if (base == 0U) return Result::Unavailable;
#if defined(_MSC_VER) && defined(_M_IX86)
    // WorkItem refcounts/pool operations are not atomic. Match the native
    // recursive CATHODE critical section, including on SEH unwinding.
    __try {
        Fn<EnterCritical>(base, 0x0091E1B0U)(nullptr);
        __try {
            record.result = StepNative(base, character, record);
        } __finally {
            Fn<LeaveCritical>(base, 0x0091E1D0U)();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        record.exceptionCode = GetExceptionCode();
        return Fail(record, Result::NativeFault);
    }
#else
    Fn<EnterCritical>(base, 0x0091E1B0U)(nullptr);
    record.result = StepNative(base, character, record);
    Fn<LeaveCritical>(base, 0x0091E1D0U)();
#endif
    // A normal pre-dispatch failure has no external equipment queue borrowing
    // an ObjectRef. Release our native holders; the failed record stays latched.
    if (record.stage == Stage::Failed && !record.giveStarted)
        ReleaseCompleted(base, record);
    return record.result;
}

void ReleaseCompleted(std::uintptr_t base, Record& record) noexcept {
    if (base == 0U || record.exceptionCode != 0U ||
        (record.stage != Stage::Equipped &&
         !(record.stage == Stage::Failed && !record.giveStarted))) return;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        Fn<EnterCritical>(base, 0x0091E1B0U)(nullptr);
        __try {
            ReleaseOwned(base, record);
        } __finally {
            Fn<LeaveCritical>(base, 0x0091E1D0U)();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        record.exceptionCode = GetExceptionCode();
    }
#else
    Fn<EnterCritical>(base, 0x0091E1B0U)(nullptr);
    ReleaseOwned(base, record);
    Fn<LeaveCritical>(base, 0x0091E1D0U)();
#endif
}

const char* Describe(Result result) noexcept {
    switch (result) {
    case Result::Pending: return "CATHODE provisioning pending; polling existing operation.";
    case Result::Equipped: return "The exact resolved equipment instance is present in the Character's native equipment table.";
    case Result::Unavailable: return "Exact retail CATHODE GiveToCharacter boundaries unavailable.";
    case Result::InvalidCharacter: return "Character runtime identity or transform is invalid.";
    case Result::InvalidCatalog: return "The selected equipment catalog is invalid or incomplete.";
    case Result::CompositeMissing: return "The selected equipment composite is absent from the loaded runtime.";
    case Result::CreateFailed: return "Selected equipment native creation or public-input boundary failed.";
    case Result::FinalizeFailed: return "Selected equipment composite finalization returned false.";
    case Result::OutputMismatch: return "The selected public equipment output did not resolve to the catalog's certified equipment node.";
    case Result::CharacterBridgeFailed: return "Native Character reference did not resolve to the requested CharacterBase.";
    case Result::BindFailed: return "Native WEAPON_GiveToCharacter creation or parameter binding failed.";
    case Result::PreflightFailed: return "GiveToCharacter Character/Weapon resolver round-trip failed; handler not dispatched.";
    case Result::DispatchRejected: return "CATHODE WEAPON_GiveToCharacter dispatch returned false.";
    case Result::SlotConflict: return "Equipment attachment conflicted with the requested native instance.";
    case Result::NativeFault: return "Native CATHODE call faulted; operation quarantined, no automatic retry.";
    case Result::TimedOut: return "CATHODE observation budget exhausted; no duplicate SPAWN or Give dispatched.";
    case Result::ResourceInitializationFailed: return "Native one-shot equipment resource initialization could not be verified; no automatic retry or refill.";
    }
    return "Unknown CATHODE provisioning result.";
}
} // namespace aimp::cathode_equipment
