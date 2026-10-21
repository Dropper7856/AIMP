using AIMP.Launcher.Contracts;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/voice")]
public sealed class VoiceController : ControllerBase
{
    [HttpGet("devices")]
    public ActionResult<IReadOnlyList<VoiceMicrophoneDeviceDto>> Devices() =>
        Ok(VoiceMicrophoneDevices.Enumerate());
}
