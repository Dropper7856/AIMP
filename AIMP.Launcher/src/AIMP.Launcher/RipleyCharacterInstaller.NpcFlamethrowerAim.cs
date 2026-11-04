using System.Numerics;
using CATHODE;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;
using CathodeLib.ObjectExtensions;

namespace AIMP.Launcher;

internal static partial class RipleyCharacterInstaller
{
    // While a remote player aims the flamethrower, other players should see
    // its pilot flame and hear the weapon. NPC_Flamethrower_Standard has
    // neither, and Base_TP has no sound object. The player archetype holds the
    // retail definitions: its pilot flame instance (wind tuning), its sound
    // events and the Flamethrower bank slot. Copy those definitions so every
    // peer publishes the same values, and drive them from the remote weapon's
    // own aim and fire outputs. The bank goes to the PLAYER_WEAPON pool, as when
    // a local player equips a flamethrower; scripts never unload these banks,
    // so a remote weapon does not evict the observer's own weapon sounds.
    private const string PlayerFlamethrowerComposite = @"Required_Assets\Archetypes\Weapons\Flamethrower_Standard";
    private const string PilotFlameComposite = @"Required_Assets\Flame_Thrower_VFX\Flame_Thrower_Constant_Flame";
    internal static readonly ShortGuid NpcFlamethrowerPilotFlameId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_PilotFlame");
    private static readonly ShortGuid NpcFlamethrowerAimingId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_Aiming");
    private static readonly ShortGuid NpcFlamethrowerAimGateId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_AimGate");
    private static readonly ShortGuid NpcFlamethrowerSoundObjectId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_SoundObject");
    private static readonly ShortGuid NpcFlamethrowerSoundBankId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_SoundBank");
    private static readonly ShortGuid NpcFlamethrowerFireLoopId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_FireLoop");
    private static readonly ShortGuid NpcFlamethrowerPilotIdleId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_PilotIdle");
    private static readonly ShortGuid NpcFlamethrowerPilotClickId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_PilotClick");

    internal static bool EnsureNpcFlamethrowerAimEffects(Commands commands)
    {
        var composite = commands.GetComposite(NpcFlamethrowerComposite);
        var player = commands.GetComposite(PlayerFlamethrowerComposite);
        var pilotVfx = commands.GetComposite(PilotFlameComposite);
        if (composite == null || player == null || pilotVfx == null) return false;
        var ids = new[]
        {
            NpcFlamethrowerPilotFlameId, NpcFlamethrowerAimingId, NpcFlamethrowerAimGateId, NpcFlamethrowerSoundObjectId,
            NpcFlamethrowerSoundBankId, NpcFlamethrowerFireLoopId, NpcFlamethrowerPilotIdleId, NpcFlamethrowerPilotClickId
        };
        var present = ids.Count(id => composite.GetEntityByID(id) != null);
        if (present == ids.Length) return false;
        if (present != 0)
            throw new InvalidDataException($"{NpcFlamethrowerComposite} has incomplete AIMP aim effects.");

        var playerPilot = SingleFunction(player, f => f.function == pilotVfx.shortGUID, "pilot flame");
        var playerBank = SingleFunction(player, f => IsFunction(f, FunctionType.SoundLoadSlot), "sound bank slot");
        var playerFireLoop = PlayerSound(player, "Play_Flamethrower_Loop");
        var playerPilotIdle = PlayerSound(player, "play_flamethrower_idle");
        var playerPilotClick = PlayerSound(player, "Play_WPN_Flamethrower_PilotLightClick");
        var weapon = SingleAimedWeapon(composite);

        FunctionEntity Add(ShortGuid id, FunctionEntity entity)
        {
            var added = composite.AddFunction(entity);
            if (added.shortGUID != id) throw new InvalidDataException("An AIMP aim effect identifier was not preserved.");
            return added;
        }
        FunctionEntity CopyOf(ShortGuid id, FunctionEntity source, params string[] excluded)
        {
            var copy = new FunctionEntity(id) { function = source.function };
            foreach (var parameter in source.parameters)
                if (!excluded.Any(name => parameter.name == ShortGuidUtils.Generate(name)))
                    copy.AddParameter(parameter.name, parameter.content.Copy(), parameter.variant, true);
            return Add(id, copy);
        }

        // The player's instance starts hidden and is revealed by its Master;
        // the remote copy is displayed whenever its owner shows the weapon.
        var pilot = CopyOf(NpcFlamethrowerPilotFlameId, playerPilot, "disable_display");
        var aiming = Add(NpcFlamethrowerAimingId, new FunctionEntity(NpcFlamethrowerAimingId, FunctionType.VariableBool));
        var aimGate = Add(NpcFlamethrowerAimGateId, new FunctionEntity(NpcFlamethrowerAimGateId, FunctionType.LogicGate));
        var soundObject = Add(NpcFlamethrowerSoundObjectId, new FunctionEntity(NpcFlamethrowerSoundObjectId, FunctionType.SoundObject));
        soundObject.AddParameter("position", new cTransform(new Vector3(0.679508f, 0f, 0.0724821f), Vector3.Zero), ParameterVariant.PARAMETER, true);
        var bank = CopyOf(NpcFlamethrowerSoundBankId, playerBank);
        var fireLoop = CopyOf(NpcFlamethrowerFireLoopId, playerFireLoop);
        var pilotIdle = CopyOf(NpcFlamethrowerPilotIdleId, playerPilotIdle);
        var pilotClick = CopyOf(NpcFlamethrowerPilotClickId, playerPilotClick);
        foreach (var sound in new[] { fireLoop, pilotIdle, pilotClick })
            sound.AddParameterLink("attached_sound_object", soundObject, "reference");
        aimGate.AddParameterLink("allow", aiming, "reference");

        weapon.AddParameterLink("equipped", bank, "load_bank");

        // Aiming lights the pilot; firing replaces it with the jet and the
        // pilot returns when firing ends, but only if the player still aims.
        weapon.AddParameterLink("on_started_aiming", aiming, "set_true");
        weapon.AddParameterLink("on_started_aiming", pilotClick, "start");
        weapon.AddParameterLink("on_started_aiming", pilot, "StartConstantFlame");
        weapon.AddParameterLink("on_started_aiming", pilotIdle, "start");
        weapon.AddParameterLink("on_shooting_finished", aimGate, "trigger");
        aimGate.AddParameterLink("on_allowed", pilot, "StartConstantFlame");
        aimGate.AddParameterLink("on_allowed", pilotIdle, "start");
        foreach (var stop in new[] { "on_stopped_aiming", "unequipped", "on_discard", "despawned" })
            weapon.AddParameterLink(stop, aiming, "set_false");
        foreach (var stop in new[] { "on_shooting_started", "on_stopped_aiming", "on_fired_fail", "unequipped", "on_discard", "despawned" })
        {
            weapon.AddParameterLink(stop, pilot, "StopConstantFlame");
            weapon.AddParameterLink(stop, pilotIdle, "stop");
        }

        weapon.AddParameterLink("on_shooting_started", fireLoop, "start");
        foreach (var stop in new[] { "on_shooting_finished", "on_fired_fail", "unequipped", "on_discard", "despawned" })
            weapon.AddParameterLink(stop, fireLoop, "stop");
        return true;
    }

    private static bool IsFunction(FunctionEntity function, FunctionType type) =>
        function.function.IsFunctionType && function.function.AsFunctionType == type;

    private static FunctionEntity PlayerSound(Composite player, string soundEvent) =>
        SingleFunction(player, f => IsFunction(f, FunctionType.Sound) &&
            f.GetParameter("sound_event")?.content is cString value &&
            string.Equals(value.value, soundEvent, StringComparison.Ordinal), soundEvent);

    private static FunctionEntity SingleFunction(Composite composite, Func<FunctionEntity, bool> predicate, string description)
    {
        var matches = composite.functions.Where(predicate).ToArray();
        if (matches.Length != 1)
            throw new InvalidDataException($"{composite.name} does not have exactly one {description}.");
        return matches[0];
    }
}
