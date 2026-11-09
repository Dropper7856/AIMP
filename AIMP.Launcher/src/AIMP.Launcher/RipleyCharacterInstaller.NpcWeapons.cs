using System.Numerics;
using CATHODE;
using CATHODE.Scripting;
using CathodeLib.ObjectExtensions;

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

    // The same archetype has no flame effects either: retail NPCs never carry
    // a flamethrower, so other players saw a silent, flameless weapon. Reuse
    // the player's jet and finish effects at their retail muzzle offsets. The
    // jet composite also drives full-screen bloom and a red colour grade meant
    // for the shooter's own view; on an observer they would flash the whole
    // screen whenever anyone fires, anywhere on the map. Remote players use a
    // copy without those three post-process entities. Its AIMP name keeps it
    // out of WorldSync: each peer runs the effect locally from its own proxy.
    internal const string RemoteFlameComposite = @"AIMP\Remote_Flame_Thrower";
    private const string FlameThrowerVfxComposite = @"Required_Assets\Flame_Thrower_VFX\Flame_Thrower";
    private const string FlameThrowerFinishComposite = @"Required_Assets\Flame_Thrower_VFX\Flame_Thrower_Finish";
    internal static readonly ShortGuid RemoteFlameCompositeId = ShortGuidUtils.Generate(RemoteFlameComposite);
    internal static readonly ShortGuid NpcFlamethrowerFlameId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_Flame");
    internal static readonly ShortGuid NpcFlamethrowerFinishId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_FlameFinish");
    private static readonly FunctionType[] ShooterViewEffects = [FunctionType.BloomSettings, FunctionType.ColourSettings];

    private static bool EnsureNpcWeaponScripts(Assets assets) => EnsureNpcWeaponScripts(assets.CommandSources);

    private static bool EnsureNpcWeaponScripts(IEnumerable<Commands> sources) =>
        sources.Aggregate(false, (changed, commands) =>
            EnsureNpcFlamethrowerNotifier(commands) | EnsureNpcFlamethrowerFlames(commands) |
            EnsureNpcFlamethrowerAimEffects(commands) | EnsureNpcFlamethrowerAttachments(commands) |
            EnsureNpcFlamethrowerAnimationContext(commands) | EnsureRemoteTorch(commands) | EnsureAnimationProps(commands) | changed);

    // Character animation sets only give NPCs a stub WEAPON_HEAVY set: 22 clips
    // with a single centred aim pose, against 104 for WEAPON_SHOTGUN with the
    // full directional aim grid. A remote player aiming the flamethrower up,
    // down or sideways blended missing poses: twisted arms and the weapon
    // pointing up behind the character. Remote flamethrowers use the two-handed
    // shotgun set; the local player's first-person flamethrower is unchanged.
    private const string RemoteFlamethrowerAnimationContext = "WEAPON_SHOTGUN";

    internal static bool EnsureNpcFlamethrowerAnimationContext(Commands commands)
    {
        var composite = commands.GetComposite(NpcFlamethrowerComposite);
        if (composite == null) return false;
        var context = SingleAimedWeapon(composite).GetParameter("character_animation_context")?.content as cString
            ?? throw new InvalidDataException($"{NpcFlamethrowerComposite} has no character animation context.");
        if (context.value == RemoteFlamethrowerAnimationContext) return false;
        if (context.value != "WEAPON_HEAVY")
            throw new InvalidDataException($"{NpcFlamethrowerComposite} has an unexpected animation context '{context.value}'.");
        context.value = RemoteFlamethrowerAnimationContext;
        return true;
    }

    // Only the held model (Base_TP) follows the remote player's hand; other
    // entities stay where the weapon composite was instantiated, at the spawn.
    // Flamethrower_Standard attaches its effects through an alias of a marker
    // that is attached to its model. Base_TP has no muzzle marker but shares
    // Base's model frame, so the effects attach to its ModelReference and keep
    // their muzzle offsets.
    private static readonly ShortGuid BaseTpInstanceId = new("61-2C-4D-42");
    private static readonly ShortGuid BaseTpModelId = new("F6-4E-28-37");
    internal static readonly ShortGuid NpcFlamethrowerModelAliasId = ShortGuidUtils.Generate("AIMP_NPC_Flamethrower_ModelAttachment");

    internal static bool EnsureNpcFlamethrowerAttachments(Commands commands)
    {
        var composite = commands.GetComposite(NpcFlamethrowerComposite);
        if (composite == null || composite.GetEntityByID(NpcFlamethrowerModelAliasId) != null) return false;
        var attached = new[] { NpcFlamethrowerFlameId, NpcFlamethrowerFinishId, NpcFlamethrowerPilotFlameId, NpcFlamethrowerSoundObjectId }
            .Select(composite.GetEntityByID).Where(entity => entity != null).ToArray();
        if (attached.Length == 0) return false;
        var model = composite.GetEntityByID(BaseTpInstanceId) as FunctionEntity;
        var modelComposite = model == null ? null : commands.GetComposite(model.function);
        if (modelComposite?.GetEntityByID(BaseTpModelId) is not FunctionEntity reference ||
            !reference.function.IsFunctionType || reference.function.AsFunctionType != FunctionType.ModelReference)
            throw new InvalidDataException($"{NpcFlamethrowerComposite} does not hold the expected Base_TP model.");
        var alias = composite.AddAlias(new AliasEntity(NpcFlamethrowerModelAliasId,
            [BaseTpInstanceId, BaseTpModelId, ShortGuid.Invalid]));
        if (alias.shortGUID != NpcFlamethrowerModelAliasId)
            throw new InvalidDataException("The AIMP model attachment identifier was not preserved.");
        foreach (var entity in attached)
            alias.AddParameterLink("attachment", entity, "reference");
        return true;
    }

    internal static bool EnsureNpcFlamethrowerNotifier(Commands commands)
    {
        var composite = commands.GetComposite(NpcFlamethrowerComposite);
        if (composite == null || composite.functions.Any(f => f.function == AINotifierType)) return false;
        var weapon = SingleAimedWeapon(composite);
        if (composite.functions.Any(f => f.shortGUID == NpcFlamethrowerNotifierId))
            throw new InvalidDataException($"{NpcFlamethrowerComposite} already uses the AIMP notifier identifier.");
        var notifier = composite.AddFunction(new FunctionEntity(NpcFlamethrowerNotifierId, FunctionType.Weapon_AINotifier));
        if (notifier.shortGUID != NpcFlamethrowerNotifierId)
            throw new InvalidDataException("The AIMP notifier identifier was not preserved.");
        weapon.AddParameterLink("on_fired_success", notifier, "impact");
        weapon.AddParameterLink("on_fired_success", notifier, "trigger");
        return true;
    }

    internal static bool EnsureNpcFlamethrowerFlames(Commands commands)
    {
        var composite = commands.GetComposite(NpcFlamethrowerComposite);
        var jet = commands.GetComposite(FlameThrowerVfxComposite);
        var finishVfx = commands.GetComposite(FlameThrowerFinishComposite);
        if (composite == null || jet == null || finishVfx == null) return false;
        var hasFlame = composite.functions.Any(f => f.shortGUID == NpcFlamethrowerFlameId);
        var hasFinish = composite.functions.Any(f => f.shortGUID == NpcFlamethrowerFinishId);
        if (hasFlame != hasFinish)
            throw new InvalidDataException($"{NpcFlamethrowerComposite} has incomplete AIMP flame effects.");
        if (hasFlame)
        {
            if (commands.GetComposite(RemoteFlameCompositeId) == null)
                throw new InvalidDataException($"{RemoteFlameComposite} is missing.");
            return false;
        }

        var weapon = SingleAimedWeapon(composite);
        var remote = EnsureRemoteFlameComposite(commands, jet);
        var flame = composite.AddFunction(new FunctionEntity(NpcFlamethrowerFlameId, remote));
        var finish = composite.AddFunction(new FunctionEntity(NpcFlamethrowerFinishId, finishVfx));
        if (flame.shortGUID != NpcFlamethrowerFlameId || finish.shortGUID != NpcFlamethrowerFinishId)
            throw new InvalidDataException("The AIMP flame effect identifiers were not preserved.");
        // Flamethrower_Standard's muzzle offsets; Base_TP shares Base's frame.
        flame.AddParameter("position", new cTransform(
            new Vector3(0.679508f, 0f, 0.0724821f), new Vector3(-8.53774E-06f, 90f, 89.9999f)), ParameterVariant.PARAMETER, true);
        finish.AddParameter("position", new cTransform(
            new Vector3(0.579292f, 0f, 0.0672064f), new Vector3(0f, 90f, 0f)), ParameterVariant.PARAMETER, true);

        weapon.AddParameterLink("on_shooting_started", flame, "StartFlameThrower");
        weapon.AddParameterLink("on_shooting_started", finish, "StopFlameFinish");
        weapon.AddParameterLink("on_shooting_finished", flame, "StopFlameThrower");
        weapon.AddParameterLink("on_shooting_finished", finish, "StartFlameFinish");
        foreach (var stop in new[] { "on_fired_fail", "unequipped", "on_discard", "despawned" })
            weapon.AddParameterLink(stop, flame, "StopFlameThrower");
        return true;
    }

    private static Composite EnsureRemoteFlameComposite(Commands commands, Composite jet)
    {
        var byName = commands.GetComposite(RemoteFlameComposite);
        var byGuid = commands.GetComposite(RemoteFlameCompositeId);
        if (!ReferenceEquals(byName, byGuid))
            throw new InvalidDataException($"{RemoteFlameComposite} name and identifier conflict.");
        if (byGuid != null)
        {
            if (byGuid.functions.Any(IsShooterViewEffect))
                throw new InvalidDataException($"{RemoteFlameComposite} still contains shooter view effects.");
            return byGuid;
        }

        var copy = jet.Copy();
        copy.name = RemoteFlameComposite;
        copy.shortGUID = RemoteFlameCompositeId;
        // Renderables are level resources: the copy shares the jet's emitters
        // and lights instead of duplicating them in REDS.BIN.
        foreach (var entity in jet.GetEntities())
        {
            var copiedEntity = copy.GetEntityByID(entity.shortGUID)
                ?? throw new InvalidDataException("Remote flame entity clone is incomplete.");
            var sourceResources = ResourcesOf(entity).ToArray();
            var copiedResources = ResourcesOf(copiedEntity).ToArray();
            if (sourceResources.Length != copiedResources.Length)
                throw new InvalidDataException("Remote flame resource clone is incomplete.");
            for (var i = 0; i < sourceResources.Length; i++)
            {
                copiedResources[i].RenderableInstance = sourceResources[i].RenderableInstance;
                copiedResources[i].AnimatedModel = sourceResources[i].AnimatedModel;
                copiedResources[i].CollisionMapping = sourceResources[i].CollisionMapping;
            }
        }

        var removed = copy.functions
            .Where(IsShooterViewEffect)
            .Select(f => f.shortGUID).ToHashSet();
        if (removed.Count != 3)
            throw new InvalidDataException($"{FlameThrowerVfxComposite} does not have the expected shooter view effects ({removed.Count}).");
        foreach (var entity in copy.GetEntities())
            entity.childLinks.RemoveAll(link => removed.Contains(link.linkedEntityID));
        foreach (var id in removed)
            if (!copy.RemoveFunction(id))
                throw new InvalidDataException("A shooter view effect could not be removed from the remote flame.");

        commands.Entries.Add(copy);
        return copy;
    }

    private static bool IsShooterViewEffect(FunctionEntity function) =>
        function.function.IsFunctionType && ShooterViewEffects.Contains(function.function.AsFunctionType);

    private static FunctionEntity SingleAimedWeapon(Composite composite)
    {
        var weapons = composite.functions.Where(f => f.function == AimedWeaponType).ToArray();
        if (weapons.Length != 1)
            throw new InvalidDataException($"{composite.name} does not have exactly one AIMED_WEAPON.");
        return weapons[0];
    }
}
