# Session preparation simulation hold

`SESSION_LOADING_HOLD` is an authenticated process-level GameCommand, accepted before a native gameplay generation exists. Envelope `levelGeneration` is zero and `portableDummyId` is the connection's local Launcher player ID. Arguments are strings: `active` (`1` or `0`), `sessionId` (D-format GUID), `generation` (positive lobby start generation, not native generation).

The IPC handler updates only an atomic and a mutex-protected session token. It makes no game calls. `SESSION_LOADING_APPLIED` confirms admission; `SESSION_LOADING_REJECTED` blocks start/release. A release must match session/generation. Repeated releases are idempotent. Native and UI release tombstones prevent delayed arms from resurrecting released tokens. A different session may arm after the previous hold is released; it cannot replace an active hold. Clean transport disconnect clears native ownership. There is no timeout release.

The existing SHA-256-certified MultiplayerPause patch transaction redirects 13 existing actor simulation predicates through a hold-aware shim. These include locomotion, Havok world/dynamic objects, AI activities/thinking/display/logic/combat. The shim returns the existing nonzero retail pause mask during preparation. Its x86 context/flags preservation is identical to the existing native-menu shim. No retail pause-manager state is changed.

v213 separates arming from execution. Before an active, stable gameplay world and
a registered THE_PLAYER with initialized equipment/presentation and a valid
transform exist, these queries retain their retail behavior. The v211 unconditional
synthetic pause could otherwise stop the startup work that the barrier itself
waited for. New native counters distinguish startup passes from applied actor
holds. This admission check reuses the PlayerSyncObserver/PlayerLifecycle field
chain; no new function address or executable patch site is introduced.

Launcher position sampling now resolves THE_PLAYER through the native local-player
table, instead of treating the generic Character pool's first slot as the player.
The native manifest preload does not depend on local position availability.
Every five seconds a held session logs the remaining readiness conditions.

Eight predicates retain existing behavior: cover update, path queue, raycasts, zone manager, primary/secondary animation building, IK/ragdoll and MAIN_THREAD_CHARACTER_PROCESSING. In particular, level streaming, native character creation/readiness and the GameThreadDispatcher callbacks must keep running. CATHODE message dispatch and manifest registration are not globally paused. Animation/streaming may therefore advance behind the opaque preparation UI; actor movement and decisions remain held. This deliberately is not a global game timer freeze.

v216 corrects the creation exception to that policy: the held
HAVOK_PRE_CHARACTER_UPDATE callback also skips CharacterManagerUpdate's pending
creation queue. MAIN_THREAD_CHARACTER_PROCESSING now polls only that queue through
the existing certified CharacterReadinessProducer trampoline. The layout and
iteration are the prefix of FUN_007CC080: count +0x4370, pairs of spawn key and
record index at +0x4374, records +0x230 with stride 0x1D0 (36 slots). This does
not run CharacterManagerUpdate's subsequent actor updates or synthesize Ready.
The pump requires the current stable multiplayer world and THE_PLAYER, runs only
while the matching loading hold is active, and quarantines native faults for the
generation. No new executable hook or native function address is introduced.

Spawns wait for local manifest registration before entering the engine queue.
The release barrier again requires every peer's actual native Ready mapping,
the current process/generation's loaded manifest, active world bus and no local
in-flight world transaction/dispatch/intent. v214/v215 had removed the mapping
condition to work around the paused creation queue and could display gameplay
before the remote player was initialized.

The supplied v215 Host log also has long gaps before IPC disconnect and a
transaction timeout. Those logs do not identify the blocking Launcher stack or
prove that the loading hold caused that later stall. v216 does not alter
WorldSync replication, pipe write deadlines or heartbeat timeouts. Its native
creation pump and release behavior still require Host/Client runtime validation;
only source and executable-byte checks were performed, without a build.

`SESSION_LOADING|active=1|sessionId=...|generation=...|message=...` displays a fully opaque PickupOverlay preparation layer and suppresses its native UI input handler. `active=0` removes it only for the same token. The movie responds with `SESSION_LOADING_ACK|source=pickup_overlay|phase=gameplay|active=...|sessionId=...|generation=...`. UI socket updates remain active. The Launcher must replay current preparation display when the gameplay overlay connects, await native hold acknowledgement before starting gameplay, and release only after every participant satisfies the lobby preparation barrier. Any failure remains blocked and offers disconnection through the Launcher.

Evidence: predicate RVAs and original TEST instruction signatures are retained in `AIMP.Native/src/MultiplayerPause.cpp`, verified against the existing executable profile, and installed atomically through HookManager. Dispatcher progress is visible in `GameThreadObservationHooks.cpp`: the hooked MAIN_THREAD_CHARACTER_PROCESSING always drains queued work outside the original callback. This change adds no unverified native function call or new executable offset. Source/byte contract inspection is static evidence only; no Launcher/DLL build or game execution was performed. The modified GFX resource was regenerated with FFDec and its exported script was checked.

## Launcher barrier

Production lobbies require a portable `LobbyPreparation` report from every peer. Lobby snapshots carry a selection token so reports for a previous map cannot authorize a new one. The preparation service supplies a verified warmed cache entry; gameplay never cold-compiles level archives. Start waits for compatible manifests and ordinary Ready flags.

The local native hold is acknowledged before sending the UI START_GAME. On the gameplay movie HELLO, the coordinator replays SESSION_LOADING and requires the acknowledgement from that exact command connection and start generation. Host and Client can register their local manifest concurrently as soon as their engine and gameplay context exist. A serialized load helper shares completed registration for the same session/start/native generation/binary hash; WORLD_MANIFEST_PREPARING is polled with a new request ID until final success. Old generation results cannot commit a WorldContext.

SessionPrepared is sent only after local gameplay readiness, every remote Character's native Ready mapping, active native world bus and completed world recovery. The Host collects every peer's final report and publishes SessionRelease. Each coordinator then releases only its own matching native/UI hold. Reports repeat until release to recover interrupted broadcast writes. Normal network stop, Pause Disconnect and round return clean up the local loading token. Terminal preparation failures keep the world held and show an error with an exit instruction.
