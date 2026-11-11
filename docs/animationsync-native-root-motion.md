# AnimationSync native root-motion isolation (531)

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
`AnimationSyncMovementPolicy` in CTest. The native build requires Windows MSVC
x86; this commit is not an in-game visual proof.
