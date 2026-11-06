# Alien vs Survivors — implementation branch

Status: **IN PROGRESS; not yet a playable native Alien controller**. All changes
on `feature/alien-vs-survivors`, intentionally separate from `main`.

## Game mode and host authority

- Host creates a Survival lobby with `AlienVsSurvivors=true`.
- The lobby Host alone selects `AlienPeerId`. There is never more than one Alien;
  zero selected means the Host cannot start PvP. The chosen Alien may be Host or client.
- `LobbySnapshotPayload` and `LobbyStartPayload` carry mode and selected peer.
  Normal Co-op messages default to `false`/zero.
- PvP intentionally retains the existing survivor Character roster for its normal
  players, but the Alien peer MUST NOT receive a duplicate human Character.
  Independent and roster-driven human spawners exclude the Alien.
- `RoundEnded` uses `AlienWin` when all connected survivors are dead,
  `SurvivorsWin` when a survivor escapes or a client-controlled Alien disconnects.
  The hidden technical `THE_PLAYER` of the Alien player must never count as an
  Alien death or mission completion. A future native Alien-death event needs a
  **separate authenticated PvP event**, not the normal PlayerDeath.
- Lobby and transport protocol version incremented for new PvP contracts.
- All UI changes are ActionScript source files. Compile
  `NEWFRONTENDMENU.GFX` externally before running the modified UI; AIMP does not
  include a GFX compiler.

## Alien behavior asset

The user's `AI-Playable-Alien-main.zip` provides the research reference. The
branch stores `Resources/AlienPvP/alien_behavior.apb` instead of a complete
modified `_DIRECTORY_CONTENTS.BML`.

- Binary format `APB1`, a bounded series of copy-from-retail and inserted-byte
  commands. It reconstructs the modded asset only when the **exact original**
  user's installed BML SHA-256 matches.
- `AlienPvPBehaviorInstaller` embeds that delta and verifies the exact
  reconstructed SHA-256, maintains `.aimp-original`, publishes the replacement
  using a temporary file, and restores the original for normal Co-op lobbies.
- This is prepared **inside lobby world preflight**, not at launcher startup.
  Every participant must complete preflight. Existing `WorldManifest` matching
  still runs for the level.
- Original BML SHA-256: `334CB41B70468A1DCC6C7054F391C7715C06FA7F287AB5468D24D0BEDE30D46D`.
- Modified BML SHA-256: `0307A8346C242BFB0F7253D0E4DE6B5BF73426A3B73891408BAF5530AA0671E5`.

**Important research gaps:** the provided modified BML also includes behavior
branches originally intended for facehuggers/Androids. AI-MP does **not**
implement or expose control of either. A further Alien-only semantic rewrite is
desirable, as is proving whether a game process already in the lobby reads the
updated behavior asset on the next level load. No in-game validation has yet
established the answer. This branch does not import the photo mode.

## Alien control network

The selected player, **not necessarily the Host**, originates `AlienControlIntent`.
The Host verifies source peer, selected role, SessionId, level generation,
valid bounded movement/look inputs, and strictly increasing proposal sequence.
It sends `AlienControlCommit` carrying a Host-ordered sequence to observers.
The Host is authoritative for *admission and gameplay outcomes*; it is not
the Alien's AI decision-maker.

Current wire actions: movement axes, look yaw/pitch, sprint/sneak, attack,
enter/exit vent and roar. No client can choose a different Alien by editing the
intent payload. Messages currently terminate at transport events; **there is
not yet a game-engine input producer or native command executor**.

## Required patches to reach playable

1. **Native Alien binding**. On the certified engine thread, identify one
   genuine retail Alien (NPC class 1) from a level-authored spawn. Bind to
   Character lifetime/generation, not a global NPC type or untrusted pointer.
   Do not use `NativeCharacterSpawner` until its research gate has been cleared.
   Fail closed if the map has no suitable Alien.
2. **Ownership arbitration with AISync**. For exactly the bound Alien, drain
   outstanding AI Move/attack native requests, verify exclusive ownership, and
   suppress retail AI decisions for the duration of possession. Other NPCs,
   including any non-selected Alien, continue using existing AISync. Preserve
   the normal host-brain implementation for regular Co-op.
3. **Local input capture and native locomotion**. Adapt the original
   `control.cpp` / `input_block.cpp` findings to AIMP's x86 thread/hook
   certification and `CharacterDirectStepRuntime`; do not port the mod's
   global toggles, injector or hook manager. The Alien owner may be a client;
   they should control their own character immediately while sending intents
   and reconciling Host-approved state. Do not let both the Alien player and
   AISync mutate one native movement request.
4. **Camera and hidden technical player**. Reimplement Alien-only attachment
   based on `camera.cpp`, with local presentation and safe reset. Keep
   `THE_PLAYER` alive for the engine but hidden/non-interactive; prevent local
   player equipment and animation replication for that technical avatar.
5. **Alien spawn semantics**. Bind to the Alien's real native spawn/vent-state
   rather than the ordinary human spawn. Add an explicit prepared-and-bound
   readiness receipt before releasing the loading barrier.
6. **Attacks**. Map `Attack` and target selection to the native Alien
   `ActionMeleeAttack`/paired victim cycle, with Host-validation and outcome
   replication. Never kill a player solely because an untrusted client says
   `Attack`. Preserve existing `PlayerLifeCycle` semantics for survivors.
7. **Ventilation**. Apply and observe explicit backend transitions based on
   `vent.cpp`, including native vent path binding, backstage/frontstage and
   generation-safe release. Never use a synthetic teleport as the only effect.
8. **Round and disconnect recovery**. Test Alien client/Host departure, one
   dead survivor, multiple rounds, reload, lobby return, loss of character,
   multiple Aliens in the retail world, slow/late command stream and refusal.

## Exclusions

- NO playable facehugger.
- NO playable Android.
- NO photo mode, free camera/photo effects or ImGui mod overlay.
- NO importing the original mod's Steam P2P layer: AIMP owns transport.
- NO user-facing claim that the current branch can already play as the Alien.

## Validation

Protocol/lobby tests added in `AlienPvPTests.cs`. They exercise host-controlled
roles, payload constraints, a local two-peer lobby, and proposal/commit relay.
**They have not been executed in this environment** (Windows .NET runtime,
GFX compiler and retail game engine are unavailable here).

Manual proof: on a clean supported game, compile GFX, host a PvP survival lobby,
verify the exact BML hash and backup, select each possible Alien owner, check
client snapshots and Host-only controls, then confirm the current branch blocks
duplicate *human* remote spawns. Do not use this as a playable PvP release
until native possession, camera and attacks are delivered and tested.
