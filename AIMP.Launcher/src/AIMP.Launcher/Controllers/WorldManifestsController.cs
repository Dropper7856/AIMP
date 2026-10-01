using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/world-manifests")]
public sealed class WorldManifestsController : ControllerBase
{
    private readonly WorldManifestService _manifests;

    public WorldManifestsController(WorldManifestService manifests)
    {
        _manifests = manifests;
    }

    [HttpGet("levels")]
    [ProducesResponseType<WorldLevelListResponse>(StatusCodes.Status200OK)]
    public Task<WorldLevelListResponse> GetLevels(CancellationToken cancellationToken) =>
        _manifests.DiscoverLevelsAsync(cancellationToken);

    [HttpPost("compilations")]
    [ProducesResponseType<WorldManifestCompilationJobResponse>(StatusCodes.Status202Accepted)]
    public async Task<ActionResult<WorldManifestCompilationJobResponse>> StartCompilation(
        StartWorldManifestCompilationRequest request,
        CancellationToken cancellationToken)
    {
        var job = await _manifests.StartCompilationAsync(request, cancellationToken)
            .ConfigureAwait(false);
        return AcceptedAtAction(nameof(GetCompilation), new { id = job.Id }, job);
    }

    [HttpGet("compilations")]
    [ProducesResponseType<IReadOnlyList<WorldManifestCompilationJobResponse>>(StatusCodes.Status200OK)]
    public ActionResult<IReadOnlyList<WorldManifestCompilationJobResponse>> GetCompilations() =>
        Ok(_manifests.GetJobs());

    [HttpGet("compilations/{id:guid}")]
    [ProducesResponseType<WorldManifestCompilationJobResponse>(StatusCodes.Status200OK)]
    public ActionResult<WorldManifestCompilationJobResponse> GetCompilation(Guid id) =>
        Ok(_manifests.GetJob(id));
}
