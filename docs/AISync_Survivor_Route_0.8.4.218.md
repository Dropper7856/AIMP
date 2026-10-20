# Survivor route maintenance (v218)

Launcher 0.8.4.218; DLL 0.11.4.218-survivor-route. No manifest regeneration.

Evidence: diagnostics 20261020-143457 (Host) and 20261020-133447 (Client),
the supplied AI.exe.c, and a read-only check of AI.exe instructions. No build
or game execution was performed. Regression test sources were added.

In the final Host sample the remote participant is living, visible to the
participant registry, and backed by an active sense record. The NPC is hidden,
the backstage sweep is enabled, its selected/pending target is empty and the
participant route has zero nodes. Rearming succeeded 4053 times; eligibility
adaptations ran 5684 times with no navigation exception reported. This is not
evidence of a Client-authority problem or a failed backstage enable call.

The route had 11–29 nodes in earlier samples. At approximately 20:34:01 UTC,
its goal source switched from owner to objective after the Host's death; later
samples have zero nodes. Source inspection found that the objective cache,
documented as retaining the last living Host objective, actually continued to
read the dead Host's route. v218 reads it only while that real local Character
is living, preserving any previously captured living objective. The trace does
not record the path solver's exact failure reason; it does not prove whether
the late objective itself was unreachable or the participant's start was.

FUN_006C5D70 retains an identical enabled goal without dirtying +0x454. Its
changed-goal branch writes the goal and sets +0x454/+0x455 to 1. FUN_00721270
consumes completed path results and clears a failed route to zero; with an
unchanged goal, mesh revision and participant position, an empty result can
remain cached. A stationary hiding participant need not invalidate that cache.

v218 rearms only +0x454 after two seconds of an enabled, nondegenerate empty
route with no outstanding query and no existing dirty request. The existing
FUN_00721270 submits the new query; endpoints and route/vent selection remain
native. Active queries and valid routes are never cancelled or restarted.
New diagnostics expose navigationQueryPending, navigationGoalDirty and
navigationEmptyRouteRetries. This does not bypass native vent restrictions,
force emergence, select a player, run a Client AI, or move a Character.

Portable regression tests cover empty-route retry timing, a long pending query,
successful route recovery, disabled goals and degenerate endpoints. They have
not been compiled or executed under the current no-build instruction.
