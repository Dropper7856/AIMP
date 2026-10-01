using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/commands")]
public sealed class CommandsController : ControllerBase
{
    private readonly LauncherRuntimeService _runtime;

    public CommandsController(LauncherRuntimeService runtime)
    {
        _runtime = runtime;
    }

    [HttpGet]
    [ProducesResponseType<IReadOnlyList<CommandDefinitionResponse>>(StatusCodes.Status200OK)]
    public ActionResult<IReadOnlyList<CommandDefinitionResponse>> GetAll() =>
        Ok(_runtime.GetCommands());

    [HttpGet("{name}")]
    [ProducesResponseType<CommandDefinitionResponse>(StatusCodes.Status200OK)]
    [ProducesResponseType(StatusCodes.Status404NotFound)]
    public ActionResult<CommandDefinitionResponse> Get(string name)
    {
        var command = _runtime.GetCommands().FirstOrDefault(value =>
            string.Equals(value.Name, name, StringComparison.OrdinalIgnoreCase));
        return command is null ? NotFound() : Ok(command);
    }

    [HttpPost("{name}/execute")]
    [ProducesResponseType<CommandExecutionResponse>(StatusCodes.Status200OK)]
    public Task<CommandExecutionResponse> Execute(
        string name,
        CommandExecutionRequest request,
        CancellationToken cancellationToken) =>
        _runtime.ExecuteCommandAsync(name, request.Arguments, cancellationToken);
}
