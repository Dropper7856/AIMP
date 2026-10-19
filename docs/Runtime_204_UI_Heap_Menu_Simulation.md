# 204 - GFx heap synchronization, live multiplayer menus and AISync health

Launcher: `0.8.4.204`.
DLL: `0.11.4.204-ui-heap-live-menus`.
Branch: `fix/aisync-native-execution`.

## Evidence from the October 19 diagnostics

Both archives run 203. `063558` is Host (PID 11596); `053555` is Client
(PID 9708). Existing AI.exe.c and static PE instructions were used alongside
the supplied logs. Executable SHA-256:
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
The inspection inventory is `analysis/runtime-204/static-inspection.json`.

Host: ROUND_RETURN_READY and acknowledged connection closure occur at 06:35:52.
RETURN_TO_FRONTEND completes on the engine thread. The frontend reconnects at
06:35:54; the crash follows at 06:35:56. Thus the 203 return handshake works on
this peer. Fault thread 45184 differs from engine thread 21296. Rebasing the
coherent stack gives:

`EAAEF0 -> EA7270 -> D07E90 -> D60F10 -> D60A40 -> DD4090 -> DED990`.

EAAEF0 is a native Socket read loop; EA7270 allocates an incoming event. Stack
data contains CONFIG for the Host frontend. Return address D07EF5 belongs to
the UNLOCKED allocation branch, selected by heap+64 == 0. The allocator finds
an occupied bin with a null head. The native worker queues an event; this does
not establish that frontend ActionScript callbacks execute on that worker.

Client: the engine thread 9712 faults at preferred VA E9DDB6 while destroying an
AS3 value. It treats receiver+4 == 2 as a reference-count pointer and writes to
address 2. The receiver memory instead matches two adjacent native Socket event
objects: vtable 1485E38, refcount, kind 3, array pointer, size and capacity.
This is concrete memory ownership corruption involving Socket event storage.
The Client has not sent ROUND_RETURN_READY for this round; its crash is not
caused by a completed native frontend request.

## Native GFx heap correction

CE5B40 initializes the heap critical section at +4C unconditionally. D60260 sets
the optional lock flag at +64 from the heap descriptor. Ten certified JE branches
in the six public mutators can skip the lock even though a Socket worker and
the VM share allocations. 204 keeps the existing LOCKED paths unconditionally:

| Preferred JE address | Expected bytes |
| --- | --- |
| D078FD | 74 30 |
| D0795D | 74 34 |
| D07EB2 | 74 2F |
| D07F32 | 74 42 |
| D07FD2 | 74 33 |
| D0805A | 74 46 |
| D0810A | 74 1F |
| D08173 | 74 3C |
| D08219 | 74 71 |
| D08321 | 74 4D |

Each follows `80 7E 64 00`. The JE alone becomes two NOPs, preserving the
comparison, native lock order and paired unlock. Alloc, aligned Alloc,
AutoHeapAlloc, aligned AutoHeapAlloc, Free and Realloc are covered, including
existing heaps. Free's auto-release still occurs AFTER unlocking. No live heap
descriptor is changed and no custom outer lock survives destruction of a heap.

HookManager verifies all instructions, freezes other inventoried process
threads, checks patch-site instruction pointers, makes all sites writable and
then changes the batch. Storage is prepared before freezing; page protections
are restored in reverse order. Any mismatch aborts before mutation. Protection
restoration failures retain accurate installed-patch bookkeeping and are logged.

The native HELLO now reports uiHeapSynchronized. The Launcher requires it and
withholds UI bridge responses until the native handshake completes, reducing
the first frontend's pre-injection incoming-event window. These protections
remain resident until process exit, including after Launcher shutdown.

The evidence demonstrates an unsafe concurrent allocation path and incompatible
ownership of the same kind of storage, but does not identify every earlier
corrupting instruction. Calls already inside an unlocked path at installation
and allocations before injection remain limitations. This cannot repair a heap
already corrupted earlier in the process; use a fresh process for the new DLL.

## Menus during a multiplayer round

MultiplayerPause changes 21 complete simulation TEST instructions and six mask
loads. It covers the main simulation clock/frame, AI logic/combat/locomotion,
Havok, native animation, Character updates, effects, path queue and cover updates.
CALL shims preserve registers, the required EFLAGS semantics, and x87/SSE state.
The absolute MOV signatures are built using the actual rebased module address.

Only the certified pause-menu/platform-overlay reasons 0x2000 and 0x10000 are
removed from those simulation queries. Native requested/effective pause flags
are never overwritten. UI, input focus, camera and pickup-selection predicates
retain their retail behavior. Pause and its options pages therefore keep their
input focus while the world can continue to run.

Filtering requires bound multiplayer authority, a stable generation with matching
roots, gameplay state G+8 == 4 and a gameplay mode other than 0/3. No local-player
health check is used: a dead Host retains authority. Loading, explicit scripted
pauses and other unclassified safety reasons remain active; frontend and solo
retain their original pause behavior. The module logs query/filter counts and
the last remaining pause reasons so unsupported menu cases can be distinguished
from loading. Static coverage is not a demonstration that every retail overlay
uses only the two admitted reasons.

## AISync movement correction

The latest Client reaches 9.63 m of lag before the first kill. Between 12:35:30
and :36, navigation grows from one submission/no cancellation to 35/35, without
path failures or intent fallback. Health counters record about 40 commits but
only one native hit replay. The event log does not associate each cancellation
with an individual health event, so that exact causal count remains inferred.

The code defect is direct: ApplyAuthoritativeHealth cancelled navigation for
EVERY ordered event, including CommitOnly. AISyncHealth applies CommitOnly by
the native HP setter, without an impact reaction. The caller could also mark a
pre-existing Move-start cycle as a newly created damage reaction.

204 preserves movement during non-terminal CommitOnly updates. A real hit or
terminal commit cancels movement before its FIRST invocation, not every retry.
A reaction lease is recorded only for a real hit that actually changes the
cycle pointer or serial. Paired admission, terminal health and native damage
notifications retain their existing paths. No transform following, new Client
brain or gait override is introduced; the 203 rejoin thresholds remain unchanged.

## Verification and deployment limits

Static PE comparison matches all ten heap sites and all 27 simulation sites.
The source changes were independently reviewed; git diff --check is clean.
The existing IPC handshake fixture was updated for its new required capability.
No Launcher/DLL build, test or game execution was performed. Runtime crash
resolution, uninterrupted simulation under menus and movement improvement remain
unverified until tested with the new binaries.

No GFX was changed or regenerated in this version. Deploy the 204 Launcher and
DLL together with the existing bundled UI resources. Local native HELLO gains
one capability; AISync v12 gameplay payloads and WorldManifest remain unchanged.
No WorldManifest regeneration is needed.
