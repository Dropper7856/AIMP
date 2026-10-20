using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using CATHODE;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;
using CathodeLib;
using CathodeLib.ObjectExtensions;

namespace AIMP.Launcher;

internal sealed record RipleyInstallProgress(string Level, int CompletedLevels, int TotalLevels, string Message);
internal sealed record RipleyInstallResult(int LevelsImported, int AlreadyPresent);

/// <summary>
/// Installs the retail character definition and its referenced render assets, without
/// instantiating an NPC or changing level entry points. All serializers run on staging
/// paths; originals are backed up before an atomic, recoverable publication.
/// </summary>
internal static class RipleyCharacterInstaller
{
    private const string SourceLevel = "PRODUCTION/DLC/BSPNOSTROMO_TWOTEAMS";
    private static readonly string[] RootNames = ["E_RIPLEY", "DisplayModel:E_RIPLEY"];
    private const string JournalName = "transaction.json";

    internal static string ResolveWorldDirectory(string levelDirectory)
    {
        var root = Path.GetFullPath(levelDirectory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var patched = (root.EndsWith("BSPNOSTROMO_TWOTEAMS", StringComparison.OrdinalIgnoreCase) ||
                       root.EndsWith("BSPNOSTROMO_RIPLEY", StringComparison.OrdinalIgnoreCase)) &&
                      Directory.Exists(root + "_PATCH");
        return Path.Combine(patched ? root + "_PATCH" : root, "WORLD");
    }

    internal static RipleyInstallResult Install(string gameDirectory,
        IProgress<RipleyInstallProgress>? progress, CancellationToken cancellationToken)
    {
        lock (GamePreparationSynchronization.Gate)
            return InstallCore(gameDirectory, progress, cancellationToken);
    }

    private static RipleyInstallResult InstallCore(string gameDirectory,
        IProgress<RipleyInstallProgress>? progress, CancellationToken cancellationToken)
    {
        var game = Path.GetFullPath(gameDirectory);
        AssertGameStopped();
        RecoverInterruptedTransactions(game);
        var env = Path.Combine(game, "DATA", "ENV");
        var sourcePath = Path.Combine(env, SourceLevel.Replace('/', Path.DirectorySeparatorChar));
        if (!Directory.Exists(sourcePath))
            throw new DirectoryNotFoundException("The Crew Expendable level BSPNOSTROMO_TWOTEAMS is required to prepare E_RIPLEY.");

        var global = new Global(Path.Combine(env, "GLOBAL"),
            new PAK2(Path.Combine(game, "DATA", "GLOBAL", "ANIMATION.PAK")));
        var source = new Assets(sourcePath, global);
        var roots = source.Commands.Entries.Where(c => RootNames.Contains(c.name, StringComparer.OrdinalIgnoreCase)).ToArray();
        if (!roots.Any(c => string.Equals(c.name, "DisplayModel:E_RIPLEY", StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("The source level does not contain DisplayModel:E_RIPLEY.");
        var closure = DiscoverClosure(source.Commands, roots);
        ValidateClosure(source, closure);

        var levels = WorldManifestLevelDiscovery.Discover(game)
            .Where(level => level.CanCompile)
            .Select(level => level.LevelDirectory)
            .Where(p => !p.EndsWith("_PATCH", StringComparison.OrdinalIgnoreCase))
            .OrderBy(p => Path.GetRelativePath(env, p), StringComparer.OrdinalIgnoreCase).ToArray();
        if (levels.Length == 0) throw new InvalidDataException("No playable DLC level assets were found.");
        var imported = 0;
        var present = 0;
        for (var i = 0; i < levels.Length; i++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            AssertGameStopped();
            var name = Path.GetRelativePath(env, levels[i]).Replace('\\', '/');
            progress?.Report(new(name, i, levels.Length, "Checking E_RIPLEY character assets"));
            if (string.Equals(Path.GetFullPath(levels[i]), Path.GetFullPath(sourcePath), StringComparison.OrdinalIgnoreCase))
            {
                present++;
            }
            else
            {
                var destination = new Assets(levels[i], global);
                if (ContainsValidClosure(destination, closure)) present++;
                else
                {
                    Import(source, destination, closure, cancellationToken);
                    Publish(game, levels[i], destination, global, closure, cancellationToken);
                    imported++;
                }
            }
            progress?.Report(new(name, i + 1, levels.Length, "E_RIPLEY character assets ready"));
        }
        return new(imported, present);
    }

    private static IReadOnlyList<Composite> DiscoverClosure(Commands commands, IEnumerable<Composite> roots)
    {
        var seen = new HashSet<uint>();
        var result = new List<Composite>();
        var pending = new Stack<Composite>(roots.Reverse());
        while (pending.TryPop(out var composite))
        {
            if (!seen.Add(composite.shortGUID.AsUInt32)) continue;
            result.Add(composite);
            foreach (var function in composite.functions)
            {
                if (function.function.IsFunctionType) continue;
                var nested = commands.GetComposite(function.function)
                    ?? throw new InvalidDataException($"Character dependency {function.function} is missing from {composite.name}.");
                pending.Push(nested);
            }
        }
        return result;
    }

    private static bool ContainsValidClosure(Assets destination, IReadOnlyList<Composite> required)
    {
        foreach (var commands in destination.CommandSources)
        {
            var existing = new List<Composite>();
            foreach (var source in required)
            {
                var byName = commands.GetComposite(source.name);
                var byGuid = commands.GetComposite(source.shortGUID);
                if (byName != null && byName.shortGUID != source.shortGUID)
                    throw new InvalidDataException($"Character composite name conflict: {source.name}.");
                if (byGuid != null && !string.Equals(byGuid.name, source.name, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"Character composite GUID conflict: {source.name}.");
                if (byGuid == null) return false;
                existing.Add(byGuid);
            }
            ValidateClosure(destination, existing, commands);
        }
        return true;
    }

    private static IEnumerable<ResourceReference> ResourcesOf(Entity entity)
    {
        if (entity is FunctionEntity function && function.resources != null)
            foreach (var resource in function.resources) yield return resource;
        foreach (var parameter in entity.parameters)
            if (parameter.content is cResource resources)
                foreach (var resource in resources.value) yield return resource;
    }

    private static void ValidateClosure(Assets assets, IEnumerable<Composite> composites, Commands? commandSource = null)
    {
        var commands = commandSource ?? assets.Commands;
        var renderables = 0;
        foreach (var composite in composites)
        {
            foreach (var function in composite.functions)
                if (!function.function.IsFunctionType && commands.GetComposite(function.function) == null)
                    throw new InvalidDataException($"Missing character composite dependency in {composite.name}.");
            foreach (var resource in composite.GetEntities().SelectMany(ResourcesOf))
            {
                switch (resource.resource_type)
                {
                    case ResourceType.RENDERABLE_INSTANCE:
                        if (resource.RenderableInstance is not { Count: > 0 })
                            throw new InvalidDataException($"Unresolved character renderable in {composite.name}.");
                        foreach (var element in resource.RenderableInstance)
                            ValidateElement(assets, element, new HashSet<RenderableElements.Element>(System.Collections.Generic.ReferenceEqualityComparer.Instance));
                        renderables++;
                        break;
                    case ResourceType.ANIMATED_MODEL when resource.AnimatedModel == null:
                    case ResourceType.COLLISION_MAPPING when resource.CollisionMapping == null:
                        throw new InvalidDataException($"Unresolved character resource in {composite.name}.");
                    case ResourceType.DYNAMIC_PHYSICS_SYSTEM:
                    case ResourceType.EXCLUSIVE_MASTER_STATE_RESOURCE:
                        throw new InvalidDataException($"Character dependency {resource.resource_type} cannot be safely transferred.");
                }
            }
        }
        if (renderables == 0) throw new InvalidDataException("E_RIPLEY has no resolved renderable model.");
    }

    private static void ValidateElement(Assets assets, RenderableElements.Element element, HashSet<RenderableElements.Element> seen)
    {
        if (!seen.Add(element)) return;
        if (element.Model == null || element.Material == null || assets.Models.FindModel(element.Model) == null ||
            !assets.Materials.Entries.Any(m => ReferenceEquals(m, element.Material)))
            throw new InvalidDataException("E_RIPLEY contains an unresolved model or material.");
        if (element.Material.Shader == null || !assets.Shaders.Entries.Any(s => ReferenceEquals(s, element.Material.Shader)))
            throw new InvalidDataException("E_RIPLEY contains an unresolved shader.");
        foreach (var texture in element.Material.TextureReferences)
            if (texture.Location != TexturePtr.Source.NONE && texture.Texture == null)
                throw new InvalidDataException("E_RIPLEY contains an unresolved texture.");
        foreach (var lod in element.LODs) ValidateElement(assets, lod, seen);
    }

    private static void Import(Assets source, Assets destination, IReadOnlyList<Composite> closure, CancellationToken token)
    {
        var importer = new AssetImporter(source, destination);
        foreach (var commands in destination.CommandSources)
        {
            foreach (var composite in closure)
            {
                token.ThrowIfCancellationRequested();
                var byName = commands.GetComposite(composite.name);
                var existing = commands.GetComposite(composite.shortGUID);
                if ((byName != null && byName.shortGUID != composite.shortGUID) ||
                    (existing != null && !string.Equals(existing.name, composite.name, StringComparison.OrdinalIgnoreCase)))
                    throw new InvalidDataException($"Character composite conflict: {composite.name}.");
                if (existing != null) continue;
                var copy = composite.Copy();
                foreach (var entity in composite.GetEntities())
                {
                    var copiedEntity = copy.GetEntityByID(entity.shortGUID)
                        ?? throw new InvalidDataException("Character entity clone is incomplete.");
                    var sourceResources = ResourcesOf(entity).ToArray();
                    var copiedResources = ResourcesOf(copiedEntity).ToArray();
                    if (sourceResources.Length != copiedResources.Length) throw new InvalidDataException("Character resource clone is incomplete.");
                    for (var i = 0; i < sourceResources.Length; i++) importer.ImportResource(sourceResources[i], copiedResources[i]);
                }
                commands.Entries.Add(copy);
            }
        }
        if (!ContainsValidClosure(destination, closure)) throw new InvalidDataException("Character dependency transfer is incomplete.");
    }

    private sealed class AssetImporter(Assets source, Assets destination)
    {
        private readonly Dictionary<Models.CS2, Models.CS2> _models = new(System.Collections.Generic.ReferenceEqualityComparer.Instance);
        private readonly Dictionary<Materials.Material, Materials.Material> _materials = new(System.Collections.Generic.ReferenceEqualityComparer.Instance);
        private readonly Dictionary<List<RenderableElements.Element>, List<RenderableElements.Element>> _renderables = new(System.Collections.Generic.ReferenceEqualityComparer.Instance);

        internal void ImportResource(ResourceReference original, ResourceReference copy)
        {
            switch (original.resource_type)
            {
                case ResourceType.RENDERABLE_INSTANCE:
                    copy.RenderableInstance = ImportElements(original.RenderableInstance);
                    break;
                case ResourceType.ANIMATED_MODEL:
                    copy.AnimatedModel = destination.Animations.ImportEntry(original.AnimatedModel);
                    copy.AnimatedModel.ResourceIndex = destination.Animations.Entries.IndexOf(copy.AnimatedModel);
                    break;
                case ResourceType.COLLISION_MAPPING:
                    copy.CollisionMapping = destination.CollisionMaps.ImportEntry(original.CollisionMapping);
                    break;
            }
        }

        private List<RenderableElements.Element> ImportElements(List<RenderableElements.Element> original)
        {
            if (_renderables.TryGetValue(original, out var cached)) return cached;
            var result = new List<RenderableElements.Element>();
            foreach (var element in original)
            {
                if (element.ModelLocation != PakLocation.LEVEL || element.MaterialLocation != PakLocation.LEVEL)
                    throw new InvalidDataException("Global character models cannot be transferred as level assets.");
                var copy = element.Copy();
                var model = source.Models.FindModel(element.Model) ?? throw new InvalidDataException("Character model is missing.");
                var imported = ImportModel(model);
                var found = false;
                for (var c = 0; c < model.Components.Count && !found; c++)
                    for (var l = 0; l < model.Components[c].LODs.Count && !found; l++)
                        for (var s = 0; s < model.Components[c].LODs[l].Submeshes.Count && !found; s++)
                            if (ReferenceEquals(model.Components[c].LODs[l].Submeshes[s], element.Model))
                            {
                                copy.Model = imported.Components[c].LODs[l].Submeshes[s];
                                found = true;
                            }
                if (!found) throw new InvalidDataException("Character submesh could not be mapped.");
                copy.Material = ImportMaterial(element.Material);
                result.Add(copy);
                destination.Renderables.Entries.Add(copy);
            }
            for (var i = 0; i < original.Count; i++) result[i].LODs = ImportElements(original[i].LODs);
            _renderables.Add(original, result);
            return result;
        }

        private Models.CS2 ImportModel(Models.CS2 original)
        {
            if (_models.TryGetValue(original, out var cached)) return cached;
            // The upstream source's Models.ImportEntry appends the original rather
            // than its remapped copy. Build and append that copy explicitly so our
            // ownership contract does not depend on that implementation detail.
            var copy = original.Copy();
            for (var c = 0; c < original.Components.Count; c++)
                for (var l = 0; l < original.Components[c].LODs.Count; l++)
                    for (var s = 0; s < original.Components[c].LODs[l].Submeshes.Count; s++)
                    {
                        var from = original.Components[c].LODs[l].Submeshes[s];
                        var to = copy.Components[c].LODs[l].Submeshes[s];
                        to.Material = from.Material == null ? null! : ImportMaterial(from.Material);
                        to.WeightedCollision = destination.Collisions.ImportEntry(from.WeightedCollision);
                        to.MorphAnimSet = destination.Morphs.ImportEntry(from.MorphAnimSet);
                    }
            var existing = destination.Models.Entries.FirstOrDefault(m => string.Equals(m.Name, original.Name, StringComparison.OrdinalIgnoreCase));
            if (existing != null)
            {
                // Some CathodeLib revisions include a freshly generated private
                // submesh GUID in equality. Compare public content across parses.
                if (ModelHash(existing) != ModelHash(copy)) throw new InvalidDataException($"Existing character model conflicts with the source: {original.Name}.");
                _models.Add(original, existing);
                return existing;
            }
            destination.Models.Entries.Add(copy);
            _models.Add(original, copy);
            return copy;
        }

        private Materials.Material ImportMaterial(Materials.Material original)
        {
            if (_materials.TryGetValue(original, out var cached)) return cached;
            var copy = original.Copy();
            for (var i = 0; i < original.TextureReferences.Count; i++)
            {
                var from = original.TextureReferences[i];
                copy.TextureReferences[i].Texture = from.Location == TexturePtr.Source.GLOBAL
                    ? from.Texture : destination.Textures.ImportEntry(from.Texture);
            }
            copy.Shader = destination.Shaders.ImportEntry(original.Shader);
            var existing = destination.Materials.Entries.FirstOrDefault(m => m == copy);
            if (existing != null) copy = existing;
            else destination.Materials.Entries.Add(copy);
            _materials.Add(original, copy);
            return copy;
        }
    }

    private static string ModelHash(Models.CS2 model) => Convert.ToHexString(SHA256.HashData(
        JsonSerializer.SerializeToUtf8Bytes(model, new JsonSerializerOptions
        {
            IncludeFields = true,
            NumberHandling = System.Text.Json.Serialization.JsonNumberHandling.AllowNamedFloatingPointLiterals,
        })));

    private sealed class Assets
    {
        internal readonly string Root;
        internal readonly string WorldRoot;
        internal readonly Textures Textures;
        internal readonly Shaders Shaders;
        internal readonly Collisions Collisions;
        internal readonly MorphTargets Morphs;
        internal readonly Materials Materials;
        internal readonly Models Models;
        internal readonly RenderableElements Renderables;
        internal readonly MaterialMappings Mappings;
        internal readonly CollisionMaps CollisionMaps;
        internal readonly EnvironmentAnimations Animations;
        internal readonly Commands Commands;
        internal readonly Commands? SecondaryCommands;
        internal IEnumerable<Commands> CommandSources => SecondaryCommands is { } secondary ? [Commands, secondary] : [Commands];
        internal readonly bool HasPak;
        internal readonly bool HasBin;

        internal Assets(string root, Global global)
        {
            Root = root;
            var renderable = Path.Combine(root, "RENDERABLE");
            // These two DLC maps ship an additional WORLD overlay; this is also
            // the selection made by OpenCAGE's Level loader.
            var world = ResolveWorldDirectory(root);
            WorldRoot = world;
            if (Directory.EnumerateFiles(renderable, "*.FZIP").Any() || File.Exists(Path.Combine(world, "COMMANDS.BIN.GZ")))
                throw new InvalidDataException("Compressed level formats are not supported by the PC character preparation step.");
            Textures = new(Path.Combine(renderable, "LEVEL_TEXTURES.ALL.PAK"));
            Shaders = new(Path.Combine(renderable, "LEVEL_SHADERS_DX11.PAK"));
            Collisions = new(Path.Combine(world, "COLLISION.BIN"));
            Morphs = new(Path.Combine(world, "MORPH_TARGET_DB.BIN"));
            Materials = new(Path.Combine(renderable, "LEVEL_MODELS.MTL"), global.Textures, Textures, Shaders);
            Models = new(Path.Combine(renderable, "LEVEL_MODELS.PAK"), Materials, Collisions, Morphs);
            Renderables = new(Path.Combine(world, "REDS.BIN"), Models, Materials);
            Mappings = new(Path.Combine(world, "MATERIAL_MAPPINGS.PAK"));
            CollisionMaps = new(Path.Combine(world, "COLLISION.MAP"), Materials, Mappings);
            Animations = new(Path.Combine(world, "ENVIRONMENT_ANIMATION.DAT"), global.AnimationStrings_Debug);
            HasPak = File.Exists(Path.Combine(world, "COMMANDS.PAK"));
            HasBin = File.Exists(Path.Combine(world, "COMMANDS.BIN"));
            Commands = new(Path.Combine(world, HasPak ? "COMMANDS.PAK" : "COMMANDS.BIN"), Animations, CollisionMaps, Renderables);
            if (HasPak && HasBin)
                SecondaryCommands = new(Path.Combine(world, "COMMANDS.BIN"), Animations, CollisionMaps, Renderables);
            foreach (var file in new CathodeFile[] { Textures, Shaders, Collisions, Morphs, Materials, Models, Renderables, Mappings, CollisionMaps, Animations, Commands })
                if (!file.Loaded) throw new InvalidDataException($"Unable to parse level asset: {file.Filepath}.");
            if (SecondaryCommands is { Loaded: false })
                throw new InvalidDataException($"Unable to parse level asset: {SecondaryCommands.Filepath}.");
        }

        internal void SaveTo(string stage)
        {
            Directory.CreateDirectory(Path.Combine(stage, "RENDERABLE"));
            Directory.CreateDirectory(Path.Combine(stage, "WORLD"));
            void Save(CathodeFile file)
            {
                var relative = Path.Combine(file.Filepath.StartsWith(WorldRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                    ? "WORLD" : "RENDERABLE", Path.GetFileName(file.Filepath));
                if (!file.Save(Path.Combine(stage, relative))) throw new IOException($"Failed to stage {relative}.");
            }
            // Dependency order also rebuilds serializer lookup tables used by consumers.
            Save(Textures); Save(Shaders); Save(Collisions); Save(Morphs);
            Save(Materials); Save(Mappings); Save(Models); Save(Renderables);
            Save(CollisionMaps); Save(Animations);
            // PAK and BIN can contain independent level graphs. Preserve each
            // graph's original composites and entry points when both are present.
            foreach (var commands in CommandSources)
                if (!commands.Save(Path.Combine(stage, "WORLD", Path.GetFileName(commands.Filepath))))
                    throw new IOException($"Failed to stage {Path.GetFileName(commands.Filepath)}.");
        }
    }

    private sealed record TransactionFile(string RelativePath, string StageRelativePath, bool Existed, string OriginalHash, string PublishedHash);
    private sealed record Transaction(string LevelRelativePath, string State, TransactionFile[] Files);

    private static void Publish(string game, string levelPath, Assets destination, Global global,
        IReadOnlyList<Composite> closure, CancellationToken token)
    {
        var transactionRoot = Path.Combine(game, "AIMP", "PreparationBackups", "Ripley", Guid.NewGuid().ToString("N"));
        var stage = Path.Combine(transactionRoot, "stage");
        Directory.CreateDirectory(stage);
        try { PublishCore(game, levelPath, destination, global, closure, token, transactionRoot, stage); }
        finally
        {
            // Backups and the recovery journal remain. Temporary serializer
            // output is not needed by rollback and would otherwise duplicate
            // every prepared level on disk indefinitely.
            var checkedStage = SafePath(transactionRoot, "stage");
            if (Directory.Exists(checkedStage)) Directory.Delete(checkedStage, true);
        }
    }

    private static void PublishCore(string game, string levelPath, Assets destination, Global global,
        IReadOnlyList<Composite> closure, CancellationToken token, string transactionRoot, string stage)
    {
        destination.SaveTo(stage);
        var staged = new Assets(stage, global);
        if (!ContainsValidClosure(staged, closure)) throw new InvalidDataException("Staged E_RIPLEY validation failed.");
        var files = Directory.EnumerateFiles(stage, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(stage, f)).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToArray();
        var changed = new List<TransactionFile>();
        foreach (var relative in files)
        {
            token.ThrowIfCancellationRequested();
            var original = relative.StartsWith("WORLD" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                ? SafePath(destination.WorldRoot, Path.GetFileName(relative)) : SafePath(levelPath, relative);
            var stagedFile = SafePath(stage, relative);
            var existed = File.Exists(original);
            var hash = existed ? Hash(original) : "";
            var publishedHash = Hash(stagedFile);
            if (existed && string.Equals(hash, publishedHash, StringComparison.Ordinal)) continue;
            if (existed)
            {
                var backup = SafePath(Path.Combine(transactionRoot, "original"), relative);
                Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                File.Copy(original, backup, false);
            }
            changed.Add(new(Path.GetRelativePath(game, original), relative, existed, hash, publishedHash));
        }
        var transaction = new Transaction(Path.GetRelativePath(game, levelPath), "publishing", changed.ToArray());
        WriteJournal(transactionRoot, transaction);
        try
        {
            foreach (var file in changed)
            {
                token.ThrowIfCancellationRequested();
                AssertGameStopped();
                var target = SafePath(game, file.RelativePath);
                // Refuse to overwrite a concurrent repair/modification after staging.
                if (File.Exists(target) != file.Existed || (file.Existed && Hash(target) != file.OriginalHash))
                    throw new IOException($"Level assets changed during preparation: {target}.");
                AtomicReplace(SafePath(stage, file.StageRelativePath), target);
            }
            if (!ContainsValidClosure(new Assets(levelPath, global), closure)) throw new InvalidDataException("Published E_RIPLEY validation failed.");
            WriteJournal(transactionRoot, transaction with { State = "complete" });
        }
        catch
        {
            Restore(game, transactionRoot, transaction);
            throw;
        }
    }

    private static void RecoverInterruptedTransactions(string game)
    {
        var backups = Path.Combine(game, "AIMP", "PreparationBackups", "Ripley");
        if (!Directory.Exists(backups)) return;
        foreach (var directory in Directory.EnumerateDirectories(backups))
        {
            var journal = Path.Combine(directory, JournalName);
            if (!File.Exists(journal)) continue;
            var transaction = JsonSerializer.Deserialize<Transaction>(File.ReadAllText(journal))
                ?? throw new InvalidDataException($"Invalid character preparation journal: {journal}.");
            ValidateJournal(transaction);
            var dlcPrefix = Path.GetFullPath(WorldManifestLevelDiscovery.GetDlcDirectory(game))
                .TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            if (!SafePath(game, transaction.LevelRelativePath).StartsWith(dlcPrefix, StringComparison.OrdinalIgnoreCase))
                continue;
            if (transaction.State == "publishing") Restore(game, directory, transaction);
        }
    }

    private static void Restore(string game, string directory, Transaction transaction)
    {
        AssertGameStopped();
        ValidateJournal(transaction);
        var level = SafePath(game, transaction.LevelRelativePath);
        // Preflight all targets and backups before touching any file. A concurrent
        // mod/repair is never replaced merely because a preparation step failed.
        var restores = new List<(string Target, string? Backup, TransactionFile File)>();
        foreach (var file in transaction.Files.Reverse())
        {
            var target = SafePath(game, file.RelativePath);
            var levelPrefix = level.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var patchedPrefix = level + "_PATCH" + Path.DirectorySeparatorChar + "WORLD" + Path.DirectorySeparatorChar;
            var patchedLevel = level.EndsWith("BSPNOSTROMO_TWOTEAMS", StringComparison.OrdinalIgnoreCase) ||
                               level.EndsWith("BSPNOSTROMO_RIPLEY", StringComparison.OrdinalIgnoreCase);
            if (!target.StartsWith(levelPrefix, StringComparison.OrdinalIgnoreCase) &&
                !(patchedLevel && target.StartsWith(patchedPrefix, StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException("Character recovery journal targets another level.");
            var exists = File.Exists(target);
            var currentHash = exists ? Hash(target) : "";
            if ((file.Existed && exists && currentHash == file.OriginalHash) || (!file.Existed && !exists)) continue;
            if (!exists || currentHash != file.PublishedHash)
                throw new IOException($"Character preparation recovery found another modification; the backup and journal were retained: {target}.");
            if (!file.Existed)
            {
                restores.Add((target, null, file));
                continue;
            }
            var backup = SafePath(Path.Combine(directory, "original"), file.StageRelativePath);
            if (!File.Exists(backup) || Hash(backup) != file.OriginalHash)
                throw new InvalidDataException($"Cannot restore original character preparation backup: {backup}.");
            restores.Add((target, backup, file));
        }
        foreach (var restore in restores)
        {
            if (!File.Exists(restore.Target) || Hash(restore.Target) != restore.File.PublishedHash)
                throw new IOException($"Character preparation recovery target changed; the backup and journal were retained: {restore.Target}.");
            if (restore.Backup == null) File.Delete(restore.Target);
            else AtomicReplace(restore.Backup, restore.Target);
        }
        WriteJournal(directory, transaction with { State = "rolled-back" });
    }

    private static void ValidateJournal(Transaction transaction)
    {
        static bool IsHash(string? value) => value is { Length: 64 } && value.All(Uri.IsHexDigit);
        if (string.IsNullOrWhiteSpace(transaction.LevelRelativePath) ||
            transaction.State is not ("publishing" or "complete" or "rolled-back") || transaction.Files == null)
            throw new InvalidDataException("Invalid character preparation recovery journal.");
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in transaction.Files)
            if (file == null || string.IsNullOrWhiteSpace(file.RelativePath) || string.IsNullOrWhiteSpace(file.StageRelativePath) ||
                !paths.Add(file.RelativePath) || !IsHash(file.PublishedHash) ||
                (file.Existed ? !IsHash(file.OriginalHash) : file.OriginalHash != ""))
                throw new InvalidDataException("Invalid character preparation recovery file record.");
    }

    private static void WriteJournal(string directory, Transaction transaction)
    {
        var temporary = Path.Combine(directory, JournalName + ".tmp");
        File.WriteAllText(temporary, JsonSerializer.Serialize(transaction));
        File.Move(temporary, Path.Combine(directory, JournalName), true);
    }

    private static void AtomicReplace(string source, string target)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        var temporary = target + ".aimp-" + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.Copy(source, temporary, false);
            using (var stream = new FileStream(temporary, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) stream.Flush(true);
            if (File.Exists(target)) File.Replace(temporary, target, null);
            else File.Move(temporary, target);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private static string SafePath(string root, string relative)
    {
        var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var path = Path.GetFullPath(Path.Combine(fullRoot, relative));
        if (!path.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Character asset path escapes its level.");
        for (var cursor = new FileInfo(path).Directory; cursor != null; cursor = cursor.Parent)
        {
            if (cursor.Exists && cursor.Attributes.HasFlag(FileAttributes.ReparsePoint)) throw new IOException("Character preparation does not follow directory links.");
            if (string.Equals(cursor.FullName.TrimEnd(Path.DirectorySeparatorChar), fullRoot.TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase)) break;
        }
        if ((File.Exists(path) || Directory.Exists(path)) && File.GetAttributes(path).HasFlag(FileAttributes.ReparsePoint))
            throw new IOException("Character preparation does not follow file links.");
        return path;
    }

    private static string Hash(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    private static void AssertGameStopped()
    {
        var processes = Process.GetProcessesByName("AI");
        try
        {
            if (processes.Length != 0) throw new InvalidOperationException("Close Alien: Isolation before preparing character assets.");
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }
}
