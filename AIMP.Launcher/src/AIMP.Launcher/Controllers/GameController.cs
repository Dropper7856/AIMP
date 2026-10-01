using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/game")]
public sealed class GameController : ControllerBase
{
    private readonly LauncherRuntimeService _runtime;

    public GameController(LauncherRuntimeService runtime)
    {
        _runtime = runtime;
    }

    [HttpPost("launch")]
    [ProducesResponseType<GameLaunchResponse>(StatusCodes.Status200OK)]
    public Task<GameLaunchResponse> Launch(CancellationToken cancellationToken) =>
        _runtime.LaunchGameAsync(cancellationToken);
}
