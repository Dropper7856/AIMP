using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed record PreparedWorldManifest(
    string Path,
    WorldManifest Manifest,
    WorldManifestIdentity Identity,
    string Sha256,
    string BinarySha256,
    WorldEntityId ProbeEntity);

internal static class WorldManifestSessionLoader
{
    internal static PreparedWorldManifest Load(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var fullPath = Path.GetFullPath(path);
        if (!File.Exists(fullPath))
            throw new FileNotFoundException("The AIMP world manifest was not found.", fullPath);

        var manifest = WorldManifestBinaryCodec.Read(fullPath);
        var identity = new WorldManifestIdentity(
            manifest.FormatVersion,
            manifest.CompilerVersion,
            manifest.MapContentId);
        WorldReplicationValidation.Validate(identity);
        // WorldSync compatibility is based on the canonical fixed-width runtime
        // contract, not the complete .aimpwm file. Fresh CathodeLib parses may
        // legitimately vary in diagnostic metadata while producing identical
        // runtime identity/index data.
        var sha = WorldReplicationCodec.ComputeManifestCompatibilitySha256(manifest);
        var binarySha = WorldReplicationCodec.ComputeManifestSha256(fullPath);
        var runtimeWorld = manifest.Entities
            .Where(entity => entity.Domain == WorldDomain.World
                && (entity.Flags & WorldEntityFlags.RuntimeAddressable) != 0
                && entity.EntityId.IsValid);
        var probe = runtimeWorld
            // The historical probe prefers environment/zone/composite entries,
            // but automatic v12.8 world activation must not depend on any
            // object-family classification. Fall back to any runtime-addressable
            // World entity if a map does not expose those convenience flags.
            .OrderBy(entity => (entity.Flags & (WorldEntityFlags.EnvironmentPrimitive
                    | WorldEntityFlags.ZonePrimitive
                    | WorldEntityFlags.CompositeInstance)) != 0 ? 0 : 1)
            // Prefer entities carrying a non-zero provider/context key. v12.3
            // runtime validation proved that this is the normal generic
            // resolver path; root/fallback entities remain eligible after it.
            .ThenBy(entity => entity.CathodeEntityB == 0 ? 1 : 0)
            .ThenBy(entity => entity.EntityId.High)
            .ThenBy(entity => entity.EntityId.Low)
            .Select(entity => entity.EntityId)
            .FirstOrDefault();
        if (!probe.IsValid)
            throw new ProtocolException(
                "The manifest contains no runtime-addressable World entity.");
        return new PreparedWorldManifest(fullPath, manifest, identity, sha, binarySha, probe);
    }

    internal static PreparedWorldManifest LoadFromCache(WorldNetworkContext hostContext)
    {
        WorldReplicationValidation.Validate(hostContext);
        var path = WorldManifestCompiler.GetDefaultCachePath(hostContext.Manifest.MapContentId);
        var prepared = Load(path);
        if (prepared.Identity != hostContext.Manifest
            || !string.Equals(prepared.Sha256, hostContext.ManifestSha256,
                StringComparison.OrdinalIgnoreCase))
            throw new ProtocolException(
                "The cached local world manifest does not match the host manifest identity/hash.");
        return prepared;
    }
}
