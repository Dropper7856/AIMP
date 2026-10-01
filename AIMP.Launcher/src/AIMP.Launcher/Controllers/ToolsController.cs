using AIMP.Launcher.Contracts;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/tools")]
public sealed class ToolsController : ControllerBase
{
    [HttpPost("character-base")]
    [ProducesResponseType<CharacterBaseResponse>(StatusCodes.Status200OK)]
    public ActionResult<CharacterBaseResponse> CalculateCharacterBase(CharacterBaseRequest request)
    {
        if (!CharacterAddressCalculator.TryParseHex32(request.Ecx ?? string.Empty, out var ecx))
            throw new ArgumentException(
                "Enter a valid 32-bit hexadecimal ECX address.", nameof(request));
        if (ecx < CharacterAddressCalculator.CharacterOffset)
            throw new ArgumentException(
                "ECX is smaller than the Character offset 0x3570.", nameof(request));
        var characterBase = CharacterAddressCalculator.Calculate(ecx);
        return Ok(new CharacterBaseResponse(
            CharacterAddressCalculator.Format(ecx),
            CharacterAddressCalculator.Format(characterBase),
            (characterBase & 0x0FU) == 0U));
    }
}
