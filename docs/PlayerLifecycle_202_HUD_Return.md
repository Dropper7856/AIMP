# PlayerLifecycle 202: game-over HUD and frontend return

Launcher: `0.8.4.202`.
DLL: `0.11.4.202-playerlifecycle-hud-return`.
Branch: `fix/aisync-native-execution`.

## Recorded evidence

Client archive: `AIMP-Diagnostics-20261018-134422.zip`.
The archive reports Launcher/DLL 200, so it does not evaluate the Pause changes
or first-fault recorder from 201. The game-over HUD code was still unchanged in
201, however, and this patch addresses that separate path.

At 13:44:10 the Launcher reports AllPlayersDead, a 5000 ms return delay and
ROUND_END_ACK/displayed from PickupOverlay. At 13:44:15 it closes the world bus
and submits RETURN_TO_FRONTEND; IPC then closes. No completed native return is
recorded. This neither proves that the native return started nor excludes it.
The user's observed three-second onset cannot be resolved precisely from the
available log timestamps and mixed crash report.

Fault thread 1424 differs from engine thread 6512. With loaded image base
0x510000, EIP 0xEE40C2 maps to preferred VA 0xDD40C2. In FUN_00DD4090, the
allocator requests 32 bytes and obtains a non-null block, but its metadata
pointer at block+8 is null; `mov eax,[eax+0C]` reads address 0xC. The stack is
mixed from stack[7], so it cannot identify a reliable caller. The earlier
corrupting write and the identity of the faulting worker remain unknown.
There is no PAUSE_MENU_HELLO in this session.

Evidence: supplied logs/crash, existing AI.exe.c export and static PE instruction
inspection. AI.exe SHA-256:
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.

## Changes

PickupOverlay's socket callbacks now verify their originating socket and only
collect connection/error flags and a bounded receive buffer. ENTER_FRAME drains
complete messages in FIFO order, including native CATHODE commands and their
acknowledgements. Up to 64 messages are processed per frame; remaining complete
messages stay buffered. Overflow is explicitly reported and closes the bridge.
Bridge creation is deferred until after the native added-to-stage initialization.

The AIMP round-end Timer is replaced with a getTimer deadline consumed by the
same frame handler. Retail container/audio timers are preserved. The final
countdown update sends ROUND_RETURN_READY with session ID, level generation and
an opaque per-return token. The HUD then removes its AIMP frame/socket listeners
and closes its socket, retaining the final overlay until detach. Detach cleanup
also permits reuse of the same movie instance in a subsequent round.

The Launcher registers the return fence before sending ROUND_END. It waits for
the minimum round delay, matching READY and normal reader EOF on that exact
connection. Cancellation, transport exceptions and server-side connection
replacement cannot satisfy the EOF fence. READY is accepted directly by the
reader, outside the command semaphore, before its EOF event. No command gate is
held while waiting. Session/generation/token checks reject old return messages.

After the fence, round replication is cleared, the native world bus closes and
the frontend request is issued. Lobby transport remains alive. Lobby-start
handling now uses the command gate and checks its originating session, avoiding
a new start during an old return. The return fence is also checked after the
world-bus await. Clearing a session or starting a new round invalidates pending
fences. Missing HUD delivery or a missing fence after ten additional seconds
defers automatic native return and logs the reason; Pause Disconnect remains the
manual exit. There is no forced return after that timeout.

## Static verification and limits

Only PICKUPOVERLAY.GFX was regenerated with FFDec and its final script re-exported
for inspection. Its 235 tags retain identical non-script payloads; only DoABC
(tag 82) changes. Inspection output is under analysis/playerlifecycle-202.
The Pause fixes and first-fault recorder from 201 remain included.

No Launcher/DLL build, tests or game execution were performed. Moving the HUD
work to its frame and ordering transport shutdown address observable lifecycle
hazards; the archive does not prove that a socket callback ran on the faulting
worker, nor does EOF prove all internal GFx workers have stopped. This is not a
demonstrated repair of the allocator itself. Runtime crash resolution remains
unverified.

Launcher, DLL and UI resource must be deployed together. WorldManifest data and
AISync gameplay logic are unchanged; no manifest regeneration is required.
