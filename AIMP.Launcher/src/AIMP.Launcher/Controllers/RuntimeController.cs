using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/runtime")]
public sealed class RuntimeController : ControllerBase
{
    private readonly LauncherRuntimeService _runtime;

    public RuntimeController(LauncherRuntimeService runtime)
    {
        _runtime = runtime;
    }

    [HttpPost("inject")]
    [ProducesResponseType<InjectionResponse>(StatusCodes.Status200OK)]
    public Task<InjectionResponse> Inject(CancellationToken cancellationToken) =>
        _runtime.InjectAsync(cancellationToken);
}
