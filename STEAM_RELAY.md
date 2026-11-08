# Steam relay hosting (unofficial)

This branch lets players host and join **through Steam's relay network**, so the
host does not need port forwarding, a VPN or a tool like Porthole.

It uses Steamworks with app ID **480 (Spacewar)**, Valve's public test app. Every
Steam account can use it for free; while the launcher is open Steam shows you as
"playing Spacewar".

## How it works

The lobby protocol is unchanged. Only the transport underneath it changes:

- **Host:** when you create a lobby, the launcher still listens on its normal TCP
  port, **and** accepts Steam relay connections, bridging each one to that port on
  `127.0.0.1`. It also sets Steam rich presence so friends see **Join Game**.
- **Client:** accepting a Steam invite or selecting **Join Game** opens a Steam
  connection and a one-shot loopback TCP listener. The normal lobby client
  completes its compatibility checks and opens the lobby automatically.
- **Direct Connection** continues to use only the saved IP/hostname and port.
  Steam invitations never overwrite these settings.

Code: `AIMP.Launcher/src/AIMP.Launcher/SteamRelay.cs` (transport) and
`MultiplayerSessionCoordinator.SteamRelay.cs` (host/join wiring).

## Requirements

- Both players run this modified launcher backend (same build).
- Both have the **Steam client running and signed in**, with **different** Steam
  accounts (Steam cannot relay an account to itself).

## Install

1. Close AI-MP (and any leftover `AIMP.Launcher` processes in Task Manager).
2. Back up `<AI-MP folder>\resources\api`.
3. Copy the contents of the published `api` folder over `<AI-MP folder>\resources\api`,
   replacing files. Keep the existing `AIMP.NetZ.dll` and `Injector32\` (they are unchanged).

To undo, restore the backup folder.

## Finding your SteamID64

The launcher logs it at startup: `[Steam] Steam relay ready as steam:7656119xxxxxxxxxx`.
It is also the 17-digit number in your Steam profile URL
(`steamcommunity.com/profiles/7656119...`). For custom profile URLs, Steam →
Account details shows "Steam ID".

## Two-player test

**Host (player A)**

1. Start Steam, then the AI-MP launcher, then the game from the launcher.
2. Multiplayer → create a lobby. The status line shows
   `Lobby created - friends can join with steam:<A's id>`.
3. In the Steam friends list right-click B →
   **Invite to Game**.

**Joiner (player B)**

1. Start Steam, the AI-MP launcher and the game.
2. Accept A's invite or click A → **Join Game** in the friends list.
3. The launcher joins A's lobby automatically and opens the lobby screen.
   No Direct Connection action is needed. If the game frontend is not ready,
   the invitation waits until the main menu becomes ready. Start the game from
   the launcher if it is closed. Leave any current lobby/game before accepting.
4. Confirm the saved IP address and port have not changed. Leave the lobby and
   test **Direct Connection** separately with an IP address.

**Check**

- Both see each other in the lobby; ready up and start a round; move, pick items up,
  use voice chat.
- Launcher logs (`%LOCALAPPDATA%\AIMP\logs`) should show `[Steam]` lines:
  host: `Steam relay peer <B> connected; bridging to lobby port 28015.`;
  joiner: `Steam relay connected to steam:<A>.`
- Leaving the lobby logs `Relay tunnel ... closed` on both sides.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `Steam relay unavailable` in log | Steam not running or not signed in when the launcher started. Start Steam; the launcher retries every 10 s. |
| `Could not reach steam:... through Steam relay` | Host has no lobby open, host is not running this build, or wrong ID. |
| `That Steam ID is your own account` | Both PCs use the same Steam account; use two accounts. |
| Join Game in friends list launches Spacewar | The joiner's launcher was not open. Open the launcher first, then click Join Game. |
| Works by IP but not by Steam | Check both installs have `steam_api64.dll` and `Facepunch.Steamworks.Win64.dll` in `resources\api`. |

## Limitations

- Both launchers must already be running when the invitation is accepted.
- An invitation does not interrupt an existing lobby or running round.
- Each peer's receive queue is capped at 128 messages of at most 64 KiB
  (8 MiB plus one active TCP write). Overflow disconnects the peer rather than
  dropping bytes from the reliable protocol stream.
- Spacewar initialization restores the launcher's original Steam environment;
  AI.exe cannot inherit Spacewar app IDs from this integration.
- An official AI-MP update replaces `resources\api` and removes this change.
- Steam relay adds the relay's latency (usually small; it routes via Valve's nearest PoPs).

## Build 464 validation

- Eight automated Steam tests pass without requiring Steam: bounded reliable
  receive order/overflow, environment restoration, address validation/disposal,
  automatic lobby handshake, queued invite and failed-join cleanup, IP-only
  connection, concurrent invitations, and callback cancellation on shutdown.
- The complete managed suite reports 169/178 passing. All nine failing cases
  also fail on unmodified main `eca2e35`: MissionSync survival frontier/catalog
  barrier, Protocol models, posture/torch protocol version assertions, WS3
  wire/authority/manifest, launcher reset scope, native DLL priority, and the
  existing launcher body-length architecture check.
- Release native x86 and self-contained API x64/Injector32 x86 publication
  succeeded. API artifacts were delivered to both test PCs without deleting
  destination-only files. SHA-256 equality was checked for the three AIMP DLLs
  and both Steam dependencies.
- Live Steam invitation acceptance, actual Valve transport, game UI transitions,
  and a complete two-account game round still require manual in-game validation.
