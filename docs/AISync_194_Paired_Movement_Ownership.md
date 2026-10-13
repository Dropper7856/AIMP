# v194 — paired source execution and native movement ownership

## Recorded observations

Host `AIMP-Diagnostics-20261013-142409.zip` and Client
`AIMP-Diagnostics-20261013-132414.zip` both run v193. AISync installs and IPC
remains connected. Both manifests load; Host opens the CATHODE world bus.

The Client starts both victims through the native 4F path. In the first pair,
Client sequences 335/336 show source 4E, stage 2, flags 33, acquired victim 4F.
Sequence 337, one second later, shows source Idle1 while that victim is still
running. In the second pair, sequences 351/352 show the same successful
admission, then 353 shows Stop3B and 354 Idle1. Neither pair produces a Client
source graph callback. Victim callbacks arrive after the source has already
been replaced. This precedes any missing finish marker.

Native navigation works in some intervals: Client 322–324 moves approximately
3.5 m/s. Elsewhere the Host publishes no supported navigation goal and the
Client's resolved-direction fallback drifts or stalls. Client 317–321 stays
around (-35.19, -35.74) while the Host moves approximately 20 m. In another
interval, Host requested/effective gait is 1 while the last low-level tap is
0 or 3; the Client applies that obsolete tap to its navigation controller.

## Static evidence and its limits

Target AI.exe SHA-256:
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.

Cached REA navigation evidence
`ev_2c8f7c736c0a05590b9fb76c4b00f4c8a8f3db50e352be227d779ab059daee22`
contains FUN_00687190, FUN_00687690 and FUN_00688510. The scheduler callbacks
can call the native Idle transition FUN_0065FC80 with a current source 4E:
an empty/completed movement request is not itself a proof of paired completion.
These callbacks are credible competing execution owners. The v193 reports do
not record the actual call site that replaced each source; attribution remains
an inference until the new blocked-scheduler counters appear in a live test.

Cached execution evidence
`ev_c2d87787ffe26d7ebe9ddcfdcad2449019831428406800660e56905e5b15e68d`
and AI.exe.c confirm the paired stage dispatcher. The transition constructors
already invoke their +60 initializer synchronously, so v194 does not call that
initializer a second time. The graph's End_Action callback at VA 00664A96
repeats the player-controller classification; its player continuation is
00664AA9, immediately before the original E5 test. This boundary was checked
directly against PE machine code because its initial cached Ghidra lookup
did not resolve a function at 00664A10.

Retail gait writers use Character+42A8 -> +194 independently of the low-level
locomotion tap. The audit verifies the corresponding native stores/reset at
00684C28 and 00684041.

## Changes

- Scope paired movement protection to the two certified scheduler callbacks
  using thread-local owner context. Keep their native housekeeping running;
  prevent their Idle/Stop transitions from destroying an exact acquired Client
  source graph. The existing shared Idle hook delegates to AISync before its
  locomotion-slot check, since pair admission has already released that slot.
- Validate published source/victim lifetimes, current 4E/4F types and owners,
  native E7/D9 handshake, B0/68/6C links, graph presence, and E6/D8 completion.
  No outcome mutex is taken by this guard. Native failure/stage >= 6 and graph
  completion pass through. Transitions outside scheduler context pass through,
  and explicit replicated-pair teardown uses its existing original trampoline.
  HP and the source's isolated low completion-request bit are not graph ends.
- Apply the same exact human participant classification to the source
  End_Action callback, retaining its preceding lifetime test and following E5
  decision. Client local participants remain classified as participants when
  their native control state changes during a paired graph.
- Publish the Host's requested gait directly, without changing wire layout.
  Local effective gait, physical velocity and animation stay retail-owned.
- Add temporary native path convergence when a certified Host Move38/EABF80
  has no supported goal and position error exceeds 1.5 m for 250 ms. The
  authoritative pose serves as a route anchor; the native controller computes
  the local path and steering. Update the anchor after 25 cm and return to
  ordinary steering below 65 cm. Never write transforms in this path.
- Keep convergence leases separate from actual Host-goal leases. A real goal,
  stop, pair, traversal, unknown cycle, backstage state or expired motion lease
  cancels convergence. The old catastrophic transform recovery is excluded for
  this certified Move path and while native convergence is active.
- Record source graph presence and bounded scheduler-head kind/type/status,
  plus pairedIdleBlocked, pairedStopBlocked, lastPairedBlockedSchedulerRva and
  navigationConvergenceApplied/Failed.

Coverage still excludes unproven goal layouts. Static analysis identifies
kind9/RVA EB19AC as a heading/distance request, not a vector at +1C; v194 does
not reinterpret it as a MoveRequest. Unsupported state12 and rejected genuine
Host path requests require further evidence. Convergence is a bounded execution
fallback, not a replacement for complete native objective capture.

## Validation and acceptance

186 PE/source certification checks pass, including 105 literal runtime guards,
37 hook prefixes, and scheduler/stop stack-return boundaries. Independent source
review checked ABI, TLS/SEH restoration, native finish paths, lease identity and
ownership changes. No build, compiled test or game execution was performed.

Build/deploy Launcher 0.8.4.194 and
DLL 0.11.4.194-aisync-paired-movement-ownership on both machines, then restart
AI.exe. Existing WorldManifest data remains compatible.

Repeat Host approach/kill observation, then the surviving Client's encounter.
Verify continuous native approach, source and victim animations together,
native completion/release, and Host target authority after death. Reports must
show a live source graph and source callbacks during the pair. If scheduler
protection intervenes, its recorded RVA identifies the concrete retail producer.
Also test an interrupted/nonfatal pair and normal Idle/Stop to verify the guard
does not retain a completed graph. Runtime acceptance remains outstanding.
