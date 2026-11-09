using CATHODE;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;
using CathodeLib.ObjectExtensions;

namespace AIMP.Launcher;

internal static partial class RipleyCharacterInstaller
{
    // Tokens are the attachment entity's authored ShortGuid, not animation
    // names. Native capture observes the same field at Entity+0C. Each clone
    // is a visual-only graph: no interaction, inventory, camera or UI code.
    internal const string AnimationPropsPrefix = @"AIMP\AnimationProps\";
    internal static readonly ShortGuid AnimationPropCharacterId = ShortGuidUtils.Generate("AIMP_AnimationProp_Character");

    internal static bool EnsureAnimationProps(Commands commands)
    {
        var changed = false;
        var candidates = new Dictionary<uint, (Composite Source, FunctionEntity Attachment, Entity[] Targets)>();
        foreach (var composite in commands.Entries.Where(c => !c.name.StartsWith(@"AIMP\", StringComparison.OrdinalIgnoreCase)).ToArray())
        foreach (var attachment in composite.functions.Where(f => f.function.IsFunctionType && f.function.AsFunctionType == FunctionType.CharacterAttachmentNode))
        {
            var targets = attachment.childLinks
                .Where(l => l.thisParamID == ShortGuidUtils.Generate("attachment") && l.linkedParamID == ShortGuidUtils.Generate("reference"))
                .Select(l => composite.GetEntityByID(l.linkedEntityID)).OfType<FunctionEntity>()
                .Where(f => IsAnimationPropVisual(f) || !f.function.IsFunctionType).Cast<Entity>().ToArray();
            if (targets.Length == 0) continue;
            var token = attachment.shortGUID.AsUInt32;
            if (token == 0 || token == uint.MaxValue) continue;
            if (candidates.TryGetValue(token, out var previous))
            {
                // A shared authored identifier with two different definitions
                // cannot be selected by a network token. Fail closed.
                if (previous.Source.shortGUID != composite.shortGUID)
                    throw new InvalidDataException($"Ambiguous AnimationSync attachment token {token:X8}.");
                continue;
            }
            candidates.Add(token, (composite, attachment, targets));
        }

        var visualCopies = new Dictionary<uint, Composite?>();
        foreach (var (token, recipe) in candidates.OrderBy(p => p.Key))
        {
            var name = AnimationPropsPrefix + token.ToString("X8");
            if (commands.GetComposite(name) != null) continue;
            var remote = new Composite(name) { shortGUID = ShortGuidUtils.Generate(name) };
            var character = remote.AddFunction(new FunctionEntity(AnimationPropCharacterId, FunctionType.Character));
            var node = remote.AddFunction(CopyPropEntity(recipe.Attachment));
            node.childLinks.Clear();
            node.AddParameter("attach_on_reset", new cBool(false), ParameterVariant.PARAMETER, true);
            node.AddParameterLink("character", character, "reference");
            var count = 0;
            foreach (var target in recipe.Targets.OfType<FunctionEntity>())
            {
                var visual = CopyPropEntity(target);
                if (!visual.function.IsFunctionType)
                {
                    var source = commands.GetComposite(visual.function);
                    var closure = source == null ? null : EnsureVisualClosure(source, new HashSet<uint>());
                    if (closure == null) continue;
                    visual.function = closure.shortGUID;
                }
                visual.childLinks.Clear();
                MakePropVisible(visual);
                remote.AddFunction(visual);
                node.AddParameterLink("attachment", visual, "reference");
                ++count;
            }
            if (count == 0) continue;
            commands.Entries.Add(remote);
            changed = true;
        }
        return changed;

        Composite? EnsureVisualClosure(Composite source, HashSet<uint> visiting)
        {
            if (visualCopies.TryGetValue(source.shortGUID.AsUInt32, out var known)) return known;
            if (!visiting.Add(source.shortGUID.AsUInt32)) return null;
            var name = @"AIMP\AnimationPropVisual\" + source.shortGUID.AsUInt32.ToString("X8");
            var existing = commands.GetComposite(name);
            if (existing != null) return visualCopies[source.shortGUID.AsUInt32] = existing;
            var copy = new Composite(name) { shortGUID = ShortGuidUtils.Generate(name) };
            foreach (var function in source.functions)
            {
                if (!IsAnimationPropVisual(function) && function.function.IsFunctionType) continue;
                var entity = CopyPropEntity(function);
                if (!function.function.IsFunctionType)
                {
                    var nested = commands.GetComposite(function.function);
                    var visual = nested == null ? null : EnsureVisualClosure(nested, visiting);
                    if (visual == null) continue;
                    entity.function = visual.shortGUID;
                }
                copy.AddFunction(entity);
            }
            visiting.Remove(source.shortGUID.AsUInt32);
            if (copy.functions.Count == 0) return visualCopies[source.shortGUID.AsUInt32] = null;
            var kept = copy.functions.Select(f => f.shortGUID).ToHashSet();
            foreach (var entity in copy.functions)
            {
                // Preserve only internal visual attachments/material/transform
                // bindings. The gameplay graph and its public ports are absent.
                entity.childLinks.RemoveAll(l => !kept.Contains(l.linkedEntityID));
                MakePropVisible(entity);
            }
            commands.Entries.Add(copy);
            changed = true;
            return visualCopies[source.shortGUID.AsUInt32] = copy;
        }
    }

    private static bool IsAnimationPropVisual(FunctionEntity function) => function.function.IsFunctionType &&
        function.function.AsFunctionType is FunctionType.ModelReference or FunctionType.EnvironmentModelReference or FunctionType.PositionMarker;

    private static FunctionEntity CopyPropEntity(FunctionEntity source)
    {
        var copy = source.Copy();
        var originalResources = ResourcesOf(source).ToArray();
        var resources = ResourcesOf(copy).ToArray();
        if (originalResources.Length != resources.Length)
            throw new InvalidDataException("AnimationSync visual resource clone is incomplete.");
        for (var i = 0; i < resources.Length; ++i)
        {
            resources[i].RenderableInstance = originalResources[i].RenderableInstance;
            resources[i].AnimatedModel = originalResources[i].AnimatedModel;
            resources[i].CollisionMapping = originalResources[i].CollisionMapping;
        }
        return copy;
    }

    private static void MakePropVisible(FunctionEntity entity)
    {
        entity.AddParameter("disable_display", new cBool(false), ParameterVariant.PARAMETER, true);
        entity.AddParameter("show_on_reset", new cBool(true), ParameterVariant.PARAMETER, true);
        entity.AddParameter("delete_standard_collision", new cBool(true), ParameterVariant.PARAMETER, true);
        entity.AddParameter("delete_ballistic_collision", new cBool(true), ParameterVariant.PARAMETER, true);
    }
}
