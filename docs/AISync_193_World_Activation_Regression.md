# v193 — WorldSync activation timeout and AISync installation

## Evidence

The 2026-10-13 reports use Launcher 0.8.4.192 and DLL
0.11.4.192-aisync-native-pair-preparation. IPC remains connected on both peers.

Host archive `AIMP-Diagnostics-20261013-133603.zip`:

| Event (UTC) | Time |
| --- | --- |
| WORLD_MANIFEST_LOAD queued | 19:34:41.544 |
| Runtime index loaded, 604,937 entities | 19:34:45.259 |
| Capture/probe activation completed successfully | 19:35:04.574 |

The command takes 23.030 seconds. Its native index is valid; the remaining
19.315 seconds occur between index publication and command completion, where
the executor arms generic CATHODE capture and Door semantic-input probes.
The Launcher has a 20-second response deadline and abandons automatic world
activation before consuming the successful final result. `_worldActivationAttempted`
prevents another automatic attempt. The Host never publishes a ready world
context, so the Client never submits its own manifest load.

The previous v191 Host report `AIMP-Diagnostics-20261013-100146.zip` took
18.709 seconds for the same manifest (15:57:43.431 to 15:58:02.140 UTC).
Its capture/probe phase took 15.820 seconds. The old deadline was already
close to a successful normal load. The logs establish the expensive phase,
but do not identify which individual hook accounts for each delay.

Both v192 peers also report `aisync_v2_install` failure. The new native
certification at RVA `0x263900` contains an extra `00` byte in the operand
of `mov eax,[esi+0x118]`. The supplied retail binary has
`56 8B F1 8B 86 18 01 00 00 24 07 3C 01`. The runtime check rejects it
before AISync can run. The independently transcribed audit bytes were correct,
so the previous audit did not catch the incorrect runtime literal.

## Changes

- Give WORLD_MANIFEST_LOAD a bounded 120-second response budget, leaving other
  command response budgets at 20 seconds. This applies to every caller through
  the runtime controller, including automatic Host and Client activation.
- If the engine is not ready and returns a queued acknowledgement, wait for
  the manifest's final execution result with a bounded 120-second wait; do not
  treat queue acceptance as world readiness. Cancellation and timeout remove
  the waiter, and failure updates command diagnostics.
- Correct the extra byte without relaxing native installation checks.
- Audit all 104 literal MatchRetailBytes installation guards directly from
  AISync.cpp against AI.exe, in addition to the existing independent RE checks.
- Include runtimeIndexLoadMs, genericCaptureArmMs and doorSemanticProbeArmMs
  in successful manifest results to identify slow phases in future reports.

The WorldManifest schema, compiler, data, door replication and native AI
decision paths are unchanged. Existing compatible manifests need no regeneration.

## Verification and runtime acceptance

The strengthened audit reproduced exactly one v192 failure at RVA 0x263900
before correction. All 144 static checks pass after correction, including
104 runtime guards. No build or compiled test was run, as requested.

Build/deploy Launcher 0.8.4.193 and DLL
0.11.4.193-aisync-world-activation-fix on both peers, then restart AI.exe.
Expected diagnostics: successful aisync_v2_install, WORLD_MANIFEST_LOADED with
phase timings, compatible world context and automatic CATHODE world bus active
on the Host. Confirm the principal door replicates before testing AI locomotion
and Host kill observation. Runtime behavior still requires that multiplayer test.
