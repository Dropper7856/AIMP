#include "aimp/Bootstrap.h"
#include "aimp/Common.h"
#include "aimp/ProductVersion.h"

#include <Windows.h>

namespace {

DWORD WINAPI BootstrapThread(void*) noexcept {
    return aimp::Bootstrap::Instance().Run();
}

} // namespace

extern "C" void WINAPI AimpRequestShutdown() noexcept {
    aimp::Bootstrap::Instance().RequestStop();
}

extern "C" std::uint32_t WINAPI AimpGetState() noexcept {
    return aimp::Bootstrap::Instance().State();
}

extern "C" std::uint16_t WINAPI AimpProtocolVersion() noexcept {
    return aimp::kProtocolVersion;
}

extern "C" const char* WINAPI AimpDllVersion() noexcept {
    return aimp::kNativeVersion;
}

BOOL WINAPI DllMain(
    const HINSTANCE instance,
    const DWORD reason,
    void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        aimp::Bootstrap::Instance().SetModule(instance);
        HANDLE thread = CreateThread(
            nullptr,
            0,
            &BootstrapThread,
            nullptr,
            0,
            nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            return FALSE;
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // Never wait under the loader lock. The launcher should call the
        // exported shutdown request and wait for state=Stopped before unloading.
        aimp::Bootstrap::Instance().ReleaseStopEventOnDetach();
    }
    return TRUE;
}
