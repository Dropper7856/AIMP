#pragma once

#include <cstdint>
#include <string>

namespace aimp::campaign_save {

// Multiplayer campaign saves live in a launcher-owned profile directory, never
// in the player's personal Steam Cloud slots. While a profile is configured,
// ISteamRemoteStorage012 calls for retail save names (PROGRESSION, CHSV##,
// MSSV##, SPSV## .ais) are served from that directory; every other file and
// every call outside a profile pass through to Steam unchanged.
//
// Install swaps only the retail save-storage poll slot (exact build data
// pointer). The Steam vtable is patched lazily on the first configure, once
// SteamAPI is initialized. The poll slot runs on the main thread each frame
// and is where the cached native save catalogue is re-enumerated.
bool Install(std::uintptr_t moduleBase, std::string& error) noexcept;
[[nodiscard]] bool Installed() noexcept;
// True when a profile swap also reruns the retail save manager's profile load
// (frontend Continue then sees the staged checkpoint).
[[nodiscard]] bool ProfileReloadInstalled() noexcept;

struct Status {
    bool ready{};
    bool busy{};
    std::string sessionId;
    std::string profilePath;
    std::uint64_t saveSequence{};
    std::uint64_t writeRevision{};
    std::string error;
};

// Frontend only. The directory must already exist. Re-configuring the same
// session/path is idempotent and never re-enumerates.
bool Configure(const std::string& sessionId, const std::string& utf8Directory, std::string& error);
// Restores personal Steam saves and re-enumerates them. Frontend only.
bool Clear(const std::string& sessionId, std::string& error);
[[nodiscard]] Status Snapshot();
// Compact JSON used as the GameCommandResult message.
[[nodiscard]] std::string EncodeStatus(const Status& status);

} // namespace aimp::campaign_save
