using CATHODE;
using CATHODE.Scripting;

namespace AIMP.Launcher;

internal static partial class RipleyCharacterInstaller
{
    // A remote player's flamethrower is NPC_Flamethrower_Standard. Unlike
    // every player weapon script (Flamethrower_Standard, Pistol_SharedScript),
    // it has no Weapon_AINotifier, so its shots never reach AI senses: the
    // Host's Alien ignores a Client's flamethrower and keeps attacking. Add the
    // same retail wiring as Flamethrower_Standard: AIMED_WEAPON.on_fired_success
    // drives the notifier's impact and trigger inputs. The ShortGuid is fixed
    // so every peer publishes byte-identical level scripts.
    internal const string NpcFlamethrowerComposite = @"Required_Assets\Archetypes\Weapons\NPC_Flamethrower_Standard";
    internal static readonly ShortGuid NpcFlamethrowerNotifierId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_AINotifier");
    private static readonly ShortGuid AimedWeaponType = ShortGuidUtils.Generate("AIMED_WEAPON");
    private static readonly ShortGuid AINotifierType = ShortGuidUtils.Generate("Weapon_AINotifier");

    private static bool EnsureNpcWeaponScripts(Assets assets) =>
        assets.CommandSources.Aggregate(false, (changed, commands) => EnsureNpcFlamethrowerNotifier(commands) | changed);

    internal static bool EnsureNpcFlamethrowerNotifier(Commands commands)
    {
        var composite = commands.GetComposite(NpcFlamethrowerComposite);
        if (composite == null || composite.functions.Any(f => f.function == AINotifierType)) return false;
        var weapons = composite.functions.Where(f => f.function == AimedWeaponType).ToArray();
        if (weapons.Length != 1)
            throw new InvalidDataException($"{NpcFlamethrowerComposite} does not have exactly one AIMED_WEAPON.");
        if (composite.functions.Any(f => f.shortGUID == NpcFlamethrowerNotifierId))
            throw new InvalidDataException($"{NpcFlamethrowerComposite} already uses the AIMP notifier identifier.");
        var notifier = composite.AddFunction(new FunctionEntity(NpcFlamethrowerNotifierId, FunctionType.Weapon_AINotifier));
        if (notifier.shortGUID != NpcFlamethrowerNotifierId)
            throw new InvalidDataException("The AIMP notifier identifier was not preserved.");
        weapons[0].AddParameterLink("on_fired_success", notifier, "impact");
        weapons[0].AddParameterLink("on_fired_success", notifier, "trigger");
        return true;
    }
}
