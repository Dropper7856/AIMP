# Release preparation

Integrated into `main` (Launcher) and `master` (frontend).
Frontend: `F:/Alien Isolation MP/frontend/ai-mp`. Launcher 0.8.4.212 and native DLL
0.11.4.211-prepare-release must be deployed together with the updated frontend.

## Installation

Selecting AI.exe saves the installation and starts `GET/POST /api/v1/preparation`.
The English onboarding panel polls progress and allows completion only when the
selected installation is Ready. Existing installations receive the same panel.
The Launcher independently awaits preparation before starting AI.exe.

The preparation worker imports E_RIPLEY and DisplayModel:E_RIPLEY dependencies
from the installed Crew Expendable level BSPNOSTROMO_TWOTEAMS. The source DLC is
required. The installer supports the uncompressed PC asset layout; unsupported
dependencies, conflicting existing resources and compressed layouts fail with an
explicit error rather than publishing incomplete assets.

Each changed level is staged, reparsed and validated before targeted publication.
When COMMANDS.PAK and COMMANDS.BIN coexist, each original graph and its entry
points are preserved and extended independently.
Original files and transaction journals remain under
`<game>/AIMP/PreparationBackups/Ripley`. Interrupted publication is recovered on
retry. Recovery refuses to overwrite subsequent modifications by other tools.
The installer never runs while AI.exe is running.

Campaign levels are temporarily excluded. Discovery, character import, manifest
generation and lobby resolution are restricted to `DATA/ENV/PRODUCTION/DLC`.
Preparation recipe 2 invalidates older receipts that included campaign levels.
No previously modified campaign files or manifests are deleted or restored.

After import, every discovered DLC level with COMMANDS sources is compiled and its
binary manifest decoded and checked. Import and debug compilation share a lock.
Nostromo WORLD overlays use the same selection as OpenCAGE. Ready is published
only after the complete pipeline finishes. Retrying reuses completed work.

Preparation receipts live in `%LOCALAPPDATA%/AIMP/Preparation`. Unchanged asset
paths, sizes and modification times allow subsequent launches to reuse the
receipt. The selected level's COMMANDS content hashes and complete manifest
binary hash are additionally checked before every multiplayer Start. A changed
installation must be prepared again with the game closed.

## Multiplayer readiness

Every Start refreshes the lobby preparation token. Authenticated participants
report their selected level, manifest identity and canonical compatibility hash.
Host admission requires matching reports and the existing player Ready states;
stale reports cannot satisfy a new selection. Content mismatches abort Start.

Before START_GAME, the Launcher arms the native session/generation loading hold.
The gameplay PickupOverlay receives an opaque loading layer and acknowledges its
connection/session/generation. Actor simulation remains held while streaming,
navigation, animation creation, CATHODE callbacks and command pumping continue.
The retail menu pause behavior is unchanged outside the preparation interval.

Host and Client prepare native manifest indexes on separate local workers.
WORLD_MANIFEST_PREPARING is polled with fresh request IDs; only an index matching
the current process and level generation is committed by a table swap. No level
archive compilation occurs during gameplay.

Each participant sends SessionPrepared after local spawn, remote Characters,
native WorldSync activation and transaction recovery are ready. Host waits for
all current participants before authorizing SessionRelease. A release applies
only to its matching session/start generation. Disconnect and round-return paths
clear loading ownership. Failures keep the session held and display an English
reason rather than allowing an incomplete world to become playable.

## Validation boundary

Static contract and diff reviews were performed. PickupOverlay.GFX was regenerated
and re-exported with FFDec. No Launcher or native DLL build, game execution, or
installed game asset modification was performed during implementation.
Two unexecuted loopback regression tests were added for manifest admission and
the three-player release barrier, including mismatches and stale generations.

Runtime validation is still required for first preparation, retry/interruption,
map change and mismatched peers, staggered loading, game-over return and a second
round. In particular the selective native hold's interaction with every retail
level's startup graph has not been demonstrated by a game session.

## Reset launcher

Settings and the preparation screen expose Reset launcher with an explicit
confirmation. `POST /api/v1/settings/reset` requires
`{"confirmation":"RESET_LAUNCHER"}`. AI.exe must be closed and all preparation
and debug manifest compilation jobs must have finished. Launch and reset are
serialized; configuration writes and compilation admission are guarded too.

Reset removes the Launcher settings file (including persistent player ID), the
WorldManifestCache, preparation receipts, and generated `.aimpwm` files and
`WORLD_MANIFEST_*.json` exports under the selected installation's production
levels, including former campaign exports. Each removal is constrained to an
explicit data root, and filesystem links are rejected before deletion.

The frontend then clears its onboarding and access markers and reloads into the
first-launch flow. In-memory session, preparation and debug compilation state
are cleared. Game assets, game saves, diagnostics and original-file recovery
backups are retained; reset is not an uninstall of the asset import.

Reset scope and outside-path rejection regression tests were added but not run.
