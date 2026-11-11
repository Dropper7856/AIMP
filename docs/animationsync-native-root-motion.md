# AnimationSync native root-motion isolation (533)

Status: experimental; requires **two live game instances** to verify visual
alignment. This change is deliberately scoped to AnimationSync's own scripted
cycles. It does **not** alter AISync Alien movement, paired attacks or retail
NPC steering.

## Why

Retail full-body clips have a reference-motion trajectory. Previous playback
selected those original NPC clips, but also teleported the remote Character
every post-manager frame to chase the first-person player's network transform.
After the clip, an outstanding `initialPlacementPending` caused a second
immediate snap. Neither step proves the engine's native reference motion was
actually applied; both can conceal or magnify an anchoring error.

## Ownership

* **NativeRoot**: a distinct retail NPC clip is running. The game's scripted
  cycle owns translation/rotation; no network-driven Character teleport, yaw
  write, or one-time follower placement during the clip. On completion, the
  locomotion follower resumes from the actual result, with 1500 ms before
  emergency resynchronization becomes eligible again.
* **FirstPersonPath**: no distinct NPC clip exists. Preserve the old bounded
  Character network path and optional FP bone adaptation.
* **LockerVisual**: retain the existing body-only render offset and hiding path.
* **None**: the scripted cycle is not owned; ordinary PlayerSync/AISync rules
  continue. The policy tests cover owned/unowned/native/FP/hiding combinations.

NativeRoot initially places the actor using the **first received position and
heading of that transition**, not the latest position after streaming. This
is an approximation rather than a reconstruction of the true interaction
start: a first sample received late remains offset. There is no new protocol
field or peer compatibility change.

## Diagnostics to inspect

The existing `native_animation_sync_snapshot` now exposes:

* `clipSource=npc-native` or `fp-retargeted`
* `nativeRootSamples` — frames with a live native Character transform
* `nativeRootDistanceMeters` — accumulated Character translation excluding
  single-frame jumps over 0.35 m
* `nativeRootDiscontinuities` — excluded large steps (possible teleports)
* `nativeOwnerGapMeters` and `nativeOwnerMaxGapMeters` — live Character vs
  latest FP owner world-space gap
* `nativeAnchorDriftMeters` — owner distance travelled between first receipt
  and native admission

These measure **Character** displacement, not the Havok render skeleton. A
near-zero `nativeRootDistanceMeters` on an animation that should travel proves
the Character did not move, not necessarily that the rendered body did not
move. An actual model-space/root-bone trace is the next investigation if so.

## Minimum acceptance test

1. Two players, ChallengeMap16, no Alien. First confirm plain locomotion is
   unchanged and both peers are visible.
2. Test `enter_vent` once with the remote NPC nearly still at the vent. Record
   screen video and both diagnostic archives at the end.
3. Verify `clipSource=npc-native`, `applied`, `completed`, and nonzero
   `nativeRootSamples`. Compare `nativeRootDistanceMeters` with the onscreen
   displacement and `nativeOwnerMaxGapMeters`; check for a completion snap.
4. Repeat with a delayed stream load, then a non-locomoting button or
   terminal animation. Finally test a locker, which must retain its own path.
5. If native Character motion remains ~zero while the body/owner travels,
   restore positional convergence **only for clips verified to lack physical
   root motion**; do not revert to blanket per-frame FP teleporting.

The source-level policy test is registered as
`AnimationSyncMovementPolicy` in CTest, alongside
`AnimationSyncNativeTimeline`: eight native-clock projection cases adapted
from the previous standalone Claude analysis, using explicit failure checks
even when NDEBUG is defined. Both tests are header-only. The native build
requires Windows MSVC x86; this commit is not an in-game visual proof.


## 533: Spatial forensics of vent-entry mismatches

The November 11, 2026 532 Host/client diagnostics confirm `enter_vent`
used `clipSource=npc-native` and completed twice with no admission failures:
`nativeRootSamples=246`, `nativeRootDistanceMeters=5.55`,
`nativeRootDiscontinuities=5`, and `nativeOwnerMaxGapMeters=3.24`.
The other peer captured `enter_vent` three times without remote
replays. The receiving peer's locomotion flight trace also showed
**12 hard-resync applications** after normal follower reacquisition.
That is distinct from native clip admission and can produce visible snaps
after the interaction.

Offline examination of the original Havok reference frames found that the
native `FEMALE/VENT_ENTRY_UNARMED` clip traverses approximately +0.555 m
in model Y and +2.651 m in model Z over 2.333 seconds; the FP version
travels +0.510 m and +2.543 m over 2.267 seconds. These are clip-space
reference trajectories, not proof of their world-space direction or of the
rendered mesh's motion.

533 adds **no new gameplay correction**; it captures up to the last 64
engine-thread spatial samples in `native_animation_sync_snapshot`
under `nativeSpatialTrace` (tickMs, transition, clip path, graph time,
world Character XYZ, latest owner XYZ, and each heading). Samples are
recorded at approximately 125 ms intervals only while a full-body native
clip is owned. `nativeSpatialSampleCount` counts all samples collected.
The tickMs can be correlated with the existing
`remote-locomotion-flight.jsonl` hard-resync events.

For the next short two-player vent test, compare the initial and final
native Character displacement vector with the initial and final owner
displacement vector for the same transition, and compare both headings.
If world direction differs, fix the spatial basis/heading or interaction
anchor. If their directions agree but mesh enters the wall, instrument the
visual skeleton root relative to Character. If drift begins only on cycle
completion, repair the player follower reacquisition separately.

Do not assume an animation must be corrected by new teleporting while the
native world-space direction and the rendered skeleton path remain
unmeasured.
