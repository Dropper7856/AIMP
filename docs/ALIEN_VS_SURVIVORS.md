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
- `AlienPvPBehaviorInstaller` embeds that delta, verifies the exact
  reconstructed SHA-256 and supports explicit **offline** installation and
  restoration using a verified `.aimp-original` backup.
- **IMPORTANT:** Files cannot be modified safely after `AI.exe` has loaded the
  behavior graph. A running lobby now performs a **read-only** hash/backup
  preflight, and fails with an actionable error if the BML was not installed
  before game launch. Regular Co-op never rewrites it at runtime.
- The automatic, backward-compatible pre-launch preparation flow has not been
  integrated yet. Merely verifying the file on disk does not prove the live
  engine is using it. It must not be claimed that lobby-time BML installation
  is solved. The long-term approach is a neutral boot-time extension enabled
  only by the runtime PvP flag, or a fully native implementation without BML.
- Every participant completes the same lobby preflight. Existing
  `WorldManifest` matching still runs for the level.
- Original BML SHA-256: `334CB41B70468A1DCC6C7054F391C7715C06FA7F287AB5468D24D0BEDE30D46D`.
- Modified BML SHA-256: `0307A8346C242BFB0F7253D0E4DE6B5BF73426A3B73891408BAF5530AA0671E5`.

**Important research gaps:** the provided modified BML includes behavior
branches for facehuggers and Androids. AI-MP does **not** expose their control,
but the current asset delta is not yet an Alien-only semantic rewrite.
These inactive branches must be stripped or proved harmless before release.
A game process already in the lobby must never be assumed to reload BML.
This branch does not import the photo mode.

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

### Native possession lease (AVS-06 — AVS-11)

The read-only `ALIEN_PVP_PROBE` still exists as a safe diagnostic. It enumerates
only real retail Alien Characters (template class 1), excludes PlayerSync
representations, and now verifies the live Character's behaviour-tree owner
and its AI manager's owner. No user-provided pointer can select an Alien.

During an Alien vs Survivors round, the Launcher issues `ALIEN_PVP_BIND` with
the Host-selected portable `ownerPeer`, every two seconds while pending and
every five seconds after success. The native engine thread finds exactly one
retail Alien and binds an incarnation ticket, Character identity, AI tree,
session cookie and native/world generations. It does **not** spawn an Alien.
No Alien, multiple Aliens, a changed Character, or an active different owner
is rejected or deferred, never resolved by an arbitrary choice.

Only the selected Character's behaviour-manager decisions, CATHODE prepass,
coordinator and AISync host/client carriers are fenced. The existing retail
manager housekeeping remains active; ordinary NPCs and Coop remain untouched.
The lease first retires that actor's legacy BT, task executor, AIMP movement
slot and known navigation resources using certified retail cleanup paths.
It is marked **Owned** only when the native movement ownership boundary proves
quiescence with the exact incarnation and a pinned movement resource. The
boundary is re-verified for the life of the round.

`ALIEN_PVP_RELEASE` is issued before network authority unbind. It refuses to
release a living native Character if any paired or movement request still holds
its resources. If release cannot be proven before disconnection, its fence
survives the lobby configuration change until native destruction/world
generation advancement or a later safe release. The Character is never
recreated to clear a stale lease. Vent respawn is handled through a fresh
incarnation, not address reuse.

**Limitation:** This is native *ownership isolation*, not yet usable Alien
control. No input source, camera attachment, attack or vent-driving command has
been connected. The lease remains pending if the native world cannot prove
a quiescent boundary; never pretend possession succeeded.

The technical Alien player's normal human position/equipment/action
transmissions remain disabled. The Host rejects those PlayerSync packets.

## AVS-12 through AVS-18 — local Alien pilot (experimental)

A local-pilot implementation is now wired to the milestone-1 lease, with
**native retail-game proof still required**. This does not mean the PvP game
mode is playable. The native Alien must first reach `ALIEN_PVP_OWNED`, then
the local pilot must report `localOwner=1 localPilotReady=1` on a subsequent
bind poll. `ALIEN_PVP_OWNED` alone only proves the ownership boundary.

- **Owner only:** the native pilot runs only on the machine whose
  AISync `localPeer` matches the Host-selected `AlienPeerId`. It uses the
  same incarnation ticket and certified engine-thread/world scope as the lease.
  It cannot drive a remote peer's Character.
- **Input isolation:** the game executable's original USER32 IAT pointers
  are checked before install; a conflicting hook, missing RawInput or
  unexpected executable import fails closed. Only the game's input is filtered;
  the AIMP DLL reads keys through its own import. The hooks are inactive
  outside local PvP and are restored on normal release. ESC, Alt/Windows
  system keys and screenshot shortcuts remain available; raw mouse movement
  continues so the native look controller can turn. No outside-user32 global
  detours or donor hook manager are installed.
- **Camera:** only a live retail follow/head camera is reused, and only if
  the current camera is not a cutscene. It follows the exact possessed Alien;
  `V` toggles a native head-bone camera if BoneAttached can be verified.
  The Alien skull/shell submodels are temporarily hidden in head view and
  restored in follow view when the same live Character is certified. On
  release, camera targets return to local THE_PLAYER without hijacking
  a cinematic view; stale-generation cameras are never reactivated.
- **Technical Amanda visibility:** local THE_PLAYER stays alive, but the
  native submodel visibility bytes for its exact model are hidden while piloting.
  Every frame locates current submodels by native name; original visibility
  flags are retained by name, never by an old reused flags pointer. The
  original flags are restored only when the technical Character runtime ID,
  skeleton and level generation still match. Unexpected native models fail
  closed and disable Alien inputs rather than risk altering another NPC.
- **Locomotion:** WASD directions are taken relative to the local look yaw.
  CTRL is slow/sneak, SHIFT faster and SPACE run. Requests enter AIMP's
  existing native NPC-locomotion carrier with a bounded 600ms lease; no
  teleport, direct pose overwrite or global Alien AI toggle is used.
  Releasing the keys requests native Idle rather than letting a stale move
  continue. Alt-tab or the native pause menu stops the local pilot inputs.
- **Coexistence:** the periodic ownership probe recognizes the local
  controller's own movement slot, so it does not drain the player's active
  movement as an old autonomous AI request. Native entity/generation proof
  and resource pins are still required. Release first disables the pilot,
  then attempts the existing safe native boundary check.

**Unsolved / must be tested:**

- THE_PLAYER remains a technically alive Character. An Alien-owner-only,
  native submodel visibility controller is implemented, but has **not** been
  validated against live retail meshes. Direct keyboard/RawInput paths are
  blocked when possession and camera proofs hold; possible scripted inputs,
  other providers and gamepads still require a dedicated game test. No
  risky off-map player teleport is performed.
- The camera entry points and import interception were adapted using the
  provided Steam-build mod reference, not exercised in a running retail game.
  Other retail builds, controller/gamepad bindings and custom remapped keys
  are not certified.
- The local Alien movement is intentionally **not network replicated yet**;
  the client/host transport bridge belongs to milestone 3.
- Local input is not an authorization to attack or complete an objective.
  Attacks, vents and PvP consequences remain future milestones.
- The pre-launch BML preparation limitation described above remains.

### AVS milestone 2 in-game checklist

- [ ] Validate `ALIEN_PVP_OWNED` then `localPilotReady=1` for Host Alien.
- [ ] Repeat with a Client Alien; the Host must not receive local inputs.
- [ ] WASD, diagonal motion, stopping, slow/fast/run use native animations and collisions.
- [ ] Camera follows only the selected Alien; `V` switches head/follow safely
  and hides/restores the Alien's skull/shell parts when entering/leaving head view.
- [ ] Amanda's arms, head and clothing submodels are hidden on the Alien
  owner's view and restored in the next normal Co-op match.
- [ ] Amanda cannot move, aim, attack or interact while serving as the technical owner.
- [ ] ESC/pause and Alt-tab stop Alien motion and preserve normal UI behavior.
- [ ] A cinematic temporarily owns the camera without being overwritten.
- [ ] Losing an Alien during a vent transition does not dereference stale memory.
- [ ] A lobby exit/restoration leaves the original camera and game controls working.
- [ ] Two consecutive PvP rounds, then a regular Co-op match, work without reboot.
- [ ] Recheck FPS, allocation churn and input behavior when PvP is inactive.
- [ ] The Windows CI native movement-vector test succeeds and native x86
  release compilation remains green. This is a prerequisite, **not**
  proof that model/camera hooks work in the live game.

## Required patches to reach playable

1. **Native Alien binding** (AVS-06/07/08 — implementation written, game test pending).
   The engine thread identifies a retail Alien and binds an exact incarnation.
   Ambiguous/absent characters fail closed. `NativeCharacterSpawner` remains
   prohibited until its research gate is cleared.
2. **Ownership arbitration with AISync** (AVS-09/10/11 — implementation written,
   game test pending). The selected Alien's native AI decisions and replication
   are fenced, prior movement/nav resources drain, and the release fails closed.
   Prove in-game that the lease reaches Owned, only one Alien is affected and
   return-to-lobby never wakes a half-drained brain.
3. **Local input capture and native locomotion** (AVS-12–18 implementation written; game test pending). Adapt the original
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
roles, payload constraints, a local two-peer lobby, proposal/commit relay,
native probe argument rejection, victory conditions and control ordering after
a second round.
Windows CI executes the .NET tests and compiles the x86 native DLL. The retail
game engine and compiled GFX are **not** available in CI, so successful builds
cannot certify in-game AI possession or safety.

Manual proof: on a clean supported game, compile GFX, host a PvP survival lobby,
verify the exact BML hash and backup, select each possible Alien owner, check
client snapshots and Host-only controls, then confirm the current branch blocks
duplicate *human* remote spawns. Do not use this as a playable PvP release
until native possession, camera and attacks are delivered and tested.

### AVS milestone 1 manual proof checklist

- [ ] Build ActionScript into GFX externally and run a clean supported retail game.
- [ ] Co-op baseline: normal Alien AISync still operates without any PvP binding.
- [ ] PvP Host-as-Alien: exactly one retail Alien is found, its native lease reaches `ALIEN_PVP_OWNED`.
- [ ] PvP Client-as-Alien: owner ID is the Host-selected client, not automatically the Host.
- [ ] The selected Alien no longer issues autonomous decisions; all other Characters continue normally.
- [ ] Alien absent/multiple: no native possession or arbitrary actor choice.
- [ ] Vent despawn/respawn: original incarnation is retired and a new one bound.
- [ ] Disconnect/return to lobby: verified `ALIEN_PVP_RELEASED` or a retained fence; no competing AI wakeup.
- [ ] Two consecutive rounds and a level reload do not reuse stale native ownership.
- [ ] A deliberately busy navigation or native paired attack leaves lease pending instead of forcing a takeover.

Only after all checks pass should AVS-06 through AVS-11 be called **validated in game**.
