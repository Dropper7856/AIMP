# ItemSync 209 - native InventoryItem slot identity

Launcher: `0.8.4.209`. Native DLL: `0.11.4.209-itemsync-record`.
Branch: `feature/itemsync`; stable `main` is untouched.

## Observed failure in v208

The supplied archives `20261019-173438` and `20261019-183433` show
ItemSync installed and enabled. At the end of the available native logs:

| Counter | Host (173438) | Client (183433) |
| --- | ---: | ---: |
| collectSeen | 1 | 4 |
| collectNoSlot | 1 | 4 |
| observedQueued | 0 | 0 |
| applyRequests | 0 | 0 |
| ownerSpawned | 64 | 64 |

Thus the detected AddToInventory successes were all rejected by local pickup
identification. No collection report reached the network or remote removal.
The counters do not establish which individual pickup produced each success.
The v208 parent-chain and composite-instance fallback were assumptions, not a
runtime-validated identity contract. These failures must not be classified as
proof that the operations were unrelated scripted inventory gives.

## Native contract used by this change

Executable SHA-256:
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
Addresses below are preferred VAs (image base `0x00400000`); code uses RVAs.

* UI_Icon activation `0x0056B530` resolves `target_pickup_item` through
  `0x008B4E40` and stores the returned InventoryItem record at `slot+0x7C`.
* InventoryItem interface vtable slot `0x0122E6E0` points to `0x00523C80`:
  `mov eax,[ecx+0x0C]; ret 4`. It returns that same record.
* The native manager already uses this relation in `0x007762C0`:
  compare each `slot+0x7C` with the supplied record. AddToInventory uses it
  for its partial-collection path (`0x005750A7`).
* InventoryItem handler `0x0052A970` is referenced by vtable slot
  `0x0122E860`. AddToInventory sends message type 0 after granting the full
  quantity, or 0x6A after granting only part. The handler clears item data
  according to clear_on_collect, then emits collect on the complete path.
* Message IDs are the low 16 bits at `message+0x20`, as read by retail.
  `0x009306C0` adds routing flag `0x10000000`; comparing the whole word
  with zero would discard genuine complete-collection messages.

These are static observations from the existing Ghidra export, cross-checked
against the executable with Capstone. The REA MCP opened the matching artifact
with Ghidra 12.1.2, but analyze_function(0x00574EF0) timed out after 300 seconds.
No completed REA decompilation or runtime evidence is claimed for that request.
The local verification report is `analysis/itemsync/v209-static-contract.json`.

## Implementation

Replace the AddToInventory success hook with an 8-byte inline hook on the
InventoryItem handler. Its thiscall ABI has two stack arguments and RET 8;
the fastcall detour forwards both arguments and preserves the native return.
The stolen prefix consists of three complete instructions with no relative
operand. Installation checks the prologue, both RET 8 exits, the record getter,
and the relocated handler vtable pointer before installing anything.

Before retail clears the record, match exactly one live manager slot by record
pointer and copy only its owner key and position. No raw pointer is retained
across the native call or sent over IPC. Publish after the original handler
returns successfully, only while the same stable level/session is still active.

Partial messages, unrelated messages, unmatched records, ambiguous slots,
slots without a spawned composite, and untagged owners do not publish a full
removal. They have separate diagnostics. Duplicate notifications for an already
collected/applied item are ignored within the level generation. Queue rejection
no longer marks the event successfully published.

The existing Host transport and native remote removal remain the apply path;
this change does not grant inventory items on observing peers.

## Verification and remaining limits

* Offline checks passed: executable hash, nine instruction windows, two vtable
  entries, and the complete 8-byte trampoline instruction boundary.
* `git diff --check` passed.
* No build or game execution was performed. Build both v209 components before
  comparing runtime behavior; this is not a claim that the game test passed.
* Existing supported scope remains manager-spawned pickups with a portable
  owner (including ItemSync-tagged drops). Containers, untagged drops and
  partially remaining quantities are not made globally shared by this patch.
* After a full pickup, expected evidence is `collectRecordMatched` and
  `observedQueued` on the collector, followed by `applyRequests` and `applied`
  (or pendingApplied after streaming) on the observer. collectNoSpawnRef and
  collectAmbiguous explicitly distinguish unsupported/ambiguous identity.
