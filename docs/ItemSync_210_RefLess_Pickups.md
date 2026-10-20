# ItemSync 210 - pickups without a spawned-composite reference

Launcher `0.8.4.210`, native DLL `0.11.4.210-itemsync-placed`.

## Evidence from the v209 test

Both supplied archives report the v209 Launcher and DLL. The final distinct
ItemSync observations are:

| Archive | collectSeen | collectNoSlot | collectNoSpawnRef | observedQueued | applyRequests |
| --- | ---: | ---: | ---: | ---: | ---: |
| 20261019-180422 | 1 | 1 | 0 | 0 | 0 |
| 20261019-190419 | 5 | 1 | 4 | 0 | 0 |

Four complete notifications matched the actual native pickup-manager slot.
They were rejected because `slot+0x70` was null. Thus v209 fixed the record
association, but its removal contract still required a manager-spawned
composite. These counters establish the missing-reference shape, not the
complete origin/history of each object. Nothing reached the network.

## New continuation for ref-less pickups

Keep manager removal for owned spawned pickups. For a ref-less registered
pickup, preserve its native presentation by replaying the completion only:

1. Read the InventoryItem target from its retained notification reference.
2. Read the AddToInventory source from the native WorkItem at `+0x08`.
3. Validate the exact native InventoryItem/ATI interface types, the registered
   inventory record, and the shared CATHODE scope.
4. Resolve ATI's native `items` list. Require exactly one linked item, which
   must be this notification's target. Multi-item gives/containers do not
   enter this initial route; their partial/full aggregate semantics differ.
5. Send the typed item identity through the existing Host collection protocol.
6. On the observing peer resolve and retain both entities, validate their
   types, registered slot and one-item relation again.
7. Construct a real native callback, deliver InventoryItem's complete message,
   then emit ATI's success output. This clears/persists the collected marker
   and lets the original pickup graph hide models and disable its interaction.

The grant trigger `00574EF0` is never invoked by replay. The enclosing level
composite is never marked for destruction. Callback creation and all native
outputs run under RemoteRoot lineage so WorldSync does not publish them as
new local events. Both entity leases and the callback remain owned throughout
fan-out; no raw pointer crosses IPC.

## Native ABI evidence

AI.exe SHA-256:
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
Preferred VAs below use image base `00400000`; implementation uses RVAs.

* `00926FF0 -> 00926D20 -> 009265C0` retains the source reference at WorkItem
  `+08`. Message type/flags are at `+20`; routing flag `10000000` represents
  a complete message with low-word ID zero.
* ATI constructor `005299D8/DE` writes entity vtable `0122C278` and interface
  vtable `0122BEE0` at entity `+14`.
* InventoryItem's interface vtable is `0122E5E0`; handler `0052A970` is
  thiscall with two stack arguments, RET 8.
* Success helper `008D1BB0` is thiscall(iface, entity-holder, callback-holder,
  double delay), RET 10. Its callback argument is an owned native WorkItem
  holder, not a fabricated stack message.
* List getter `009065A0` is thiscall(iface, out-list-holder, source-holder),
  RET 8. Count/data are at list `+08/+0C`. Release mirrors the original
  `005751B6..005751E2`: on the last owner clear contained refs with `005F5FE0`,
  decrement the list block count, then free with `008BAA10` when it reaches 0.

These observations were checked against the cached Ghidra export and PE
instructions, including an independent native ABI/ownership review. No new
completed REA decompilation is claimed; the prior REA query timed out.

## Identity and execution guards

The new IPLC-tagged 128-bit key contains InventoryItem GUID A, scope B and ATI
GUID A. ATI and InventoryItem must share B; cross-scope item lists are not
encoded. B is computed natively from the ancestor GUID chain (`008C3260`),
not a process address. Exact manifest matches are accepted. For nested retail
scope aliases, a shared real parent and a fully readable, bounded ancestor
chain are required, rejecting known PM runtime roots with activation bit 4.
That bit is not a universal classifier for all possible dynamic spawn
families; unclassified dynamic-composite portability is not certified.

The old spawned key remains binary-compatible. Pending spawn suppression
explicitly excludes placed keys. No manifest format change or regeneration
is required for this patch.

Native callbacks run outside the ItemSync state lock. Pending retries extract
a batch before native execution, preserving entries appended by callbacks.
A bounded in-flight set defers re-entrant application of the same item.

## Validation and limits

* Offline verification passed: executable SHA-256, 12 instruction windows,
  three vtable targets and the existing inline-hook instruction boundary.
* `git diff --check` passed.
* Wire-key regression cases were added for placed IDs, invalid source/target
  identities and compatibility with spawned IDs. They were not executed.
* No build or game test was performed. Runtime disappearance, prompts and
  callback timing remain to be verified on v210.
* This route covers compatible single-item ground pickups. It does not make
  container inventories, multi-item partial collections, or unclassified
  dynamic roots generally shared.

New counters: placedMatched, placedContextRejected, placedIdentityRejected,
placedLinkRejected and placedApplied. A successful full collection should
produce placedMatched/observedQueued on the collector and placedApplied
(or pendingApplied after streaming) on the observer.
