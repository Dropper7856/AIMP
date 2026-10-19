# 205 - native Pause_Game and live menu background

Launcher: `0.8.4.205`.
DLL: `0.11.4.205-menu-simulation-render`.
Branch: `main`.

The user confirmed that a Client opening Pause still sees a stationary Host
behind the menu, despite the Host moving. Static inspection identifies two
missing native paths in 204. No new runtime diagnostic was supplied for this
report, so this is source/PE evidence rather than a measured 205 result.

## Native Pause_Game reason

The exact `Pause_Game` callback at preferred VA 00807010 sets a movie-owned
marker at +1CE2 and submits the constant at 012F3024 to 00774760. That constant
is **0x401**. `Unpause_Game` at 00807040 clears the marker and removes 0x401 and
0x8000. Escape's native frame path also adds 0x401.

204 filtered only 0x2000/0x10000. The remaining bit 1 still satisfies the native
simulation mask 0x179FF and therefore retains the paused branch.

205 admits the 0x401 pair only in a bound, stable multiplayer gameplay world
with a known menu reason or a validated movie owner. The native UI manager's
movie index (+38 array/+3C count) is inspected with bounded SEH reads and its
identity/array/count revalidated afterward. Movie +34 is the native name.
An active pause_menu or the exact hub vtable (module+EFF260) with its Pause_Game
marker proves ownership even when 0x2000/0x10000 is absent. The hub marker is
independent of visibility and remains owned until Unpause_Game.

The marker belongs to the hub movie, **not** the smaller global UI manager.
No unbounded heap search, native UI callback invocation, cached movie pointer
or write to native requested/effective flags is introduced. Unowned 0x401 and
other safety reasons remain active.

## Separate renderer pause

Native pause calls 009F22A0(renderer, false), which stores renderer+1B64 = 0.
Fourteen scene-update functions then reject ordinary updates, and A048D0 uses
its cached render queues. Unpausing simulation alone cannot guarantee a live
world behind the menu.

205 adds fifteen exact seven-byte CMP consumer sites to the existing atomic
instruction batch:

- A00CD0 + n*30, n=0..13: `80 B9 64 1B 00 00 00`, ECX renderer.
- A048D3: `80 BE 64 1B 00 00 00`, ESI renderer.

009F19C0 certifies the renderer as the static object module+15D0970. Its native
field is preserved. Only these query results allow live world updates when
the same multiplayer/menu checks prove that no simulation safety reason
remains. Input, cursor and native menu focus retain their original behavior.

The new shims reproduce `CMP AL,0`, including AF, SF and byte parity, and
preserve the remaining registers/flags plus x87/SSE state. They do not use
the 32-bit TEST semantics of the simulation queries. All 42 simulation/render
instructions install together or remain unchanged on signature mismatch.

## Verification and limits

The 15 renderer sites, 0x401 constant and renderer getter were checked directly
against the certified PE (SHA-256 00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364).
The existing simulation sites and ASLR-aware mask loads remain unchanged.
Static inventory is in `analysis/runtime-205/static-inspection.json`.

Diagnostics add pauseGameFiltered, renderQueries, renderFiltered and
uiContextRejected alongside lastMenuFlags/lastRemainingReasons. These separate
menu ownership, simulation progress and live rendering in future captures.

No Launcher/DLL build, tests or game execution were performed. Actual movement
visibility under Pause remains to be verified in game. GFX resources, AISync
gameplay payloads and WorldManifest are unchanged; no manifest regeneration is
needed. Deploy the 205 Launcher and DLL together in a fresh AI.exe process.
