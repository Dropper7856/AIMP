using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/performance/modules")]
public sealed class PerformanceController : ControllerBase
{
    private readonly LauncherRuntimeService _runtime;

    public PerformanceController(LauncherRuntimeService runtime)
    {
        _runtime = runtime;
    }

    [HttpGet]
    [ProducesResponseType<IReadOnlyList<PerformanceModuleStatus>>(StatusCodes.Status200OK)]
    public ActionResult<IReadOnlyList<PerformanceModuleStatus>> Get() =>
        Ok(_runtime.GetPerformanceModules());

    [HttpPut("all")]
    [ProducesResponseType<IReadOnlyList<PerformanceModuleStatus>>(StatusCodes.Status200OK)]
    public Task<IReadOnlyList<PerformanceModuleStatus>> SetAll(
        PerformanceUpdateRequest request,
        CancellationToken cancellationToken) =>
        _runtime.SetAllPerformanceModulesAsync(request.Enabled, cancellationToken);

    [HttpPut("{module}")]
    [ProducesResponseType<PerformanceModuleStatus>(StatusCodes.Status200OK)]
    public Task<PerformanceModuleStatus> Set(
        string module,
        PerformanceUpdateRequest request,
        CancellationToken cancellationToken) =>
        _runtime.SetPerformanceModuleAsync(module, request.Enabled, cancellationToken);
}
