using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1")]
public sealed class StatusController : ControllerBase
{
    private readonly LauncherRuntimeService _runtime;

    public StatusController(LauncherRuntimeService runtime)
    {
        _runtime = runtime;
    }

    [HttpGet("status")]
    [ProducesResponseType<LauncherStatusResponse>(StatusCodes.Status200OK)]
    public ActionResult<LauncherStatusResponse> GetStatus() => Ok(_runtime.GetStatus());

    [HttpGet("logs")]
    [ProducesResponseType<LogResponse>(StatusCodes.Status200OK)]
    public ActionResult<LogResponse> GetLogs([FromQuery] int tail = 250) =>
        Ok(_runtime.GetLogs(tail));
}
