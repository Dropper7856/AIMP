# AISync – Direct NPC locomotion (experimental)

Branch: `feature/aisync-direct-locomotion`

## Authority split

- **Host:** runs the NPC decision tree, pathfinding and physical simulation, publishes position, rotation, motion velocity, gait and flags at ~20 Hz.
- **Client:** does **not** replay native navigation kinds 1/2/3/4/6/7/8/9/11. The client feeds interpolated Host velocity, gait and pose error into the existing retail NPC locomotion/animation cycle; **native root motion and turning remain authoritative for ordinary frame-by-frame movement**. POST no longer teleports for centimetre-size drift: a physical correction is permitted only after **3.5 m** of divergence held for **750 ms**, with a **3 s** cooldown and strict ownership checks.
- **Native actions:** kind 5, reaction/state intents (including 1/2/7 as *cycle exits*, not goals), paired kills, scripts, passages, backstage/frontstage and lifecycle events remain authoritative ordered actions. No physical correction while the Host or native Client cycle owns an action.
- **Lifetime:** samples are scoped by AISync identity, birth and session/world. The timeline resets on incarnation, scope, invalid motion, and actor retirement. A 600 ms stale stream releases the motor rather than extrapolating indefinitely.
- **Other NPCs:** the controller is per eligible AI Character and not Alien-specific. The existing admitted class whitelist remains unchanged; the Facehugger (class 5, not yet certified for these native movement cycles) is deliberately not enabled blindly.

## Important remaining limitation

This first integration uses the already-shipped **v16, 912-byte causal state stream**. Motion is no longer *executed* from navigation commands, but pose delivery still shares the reliable ordered TCP lane with game actions. Under congestion, head-of-line blocking may temporarily freeze the puppet. A dedicated compact, latest-wins pose transport is a separate follow-up change, not claimed here. The 510 fix removes per-frame physics teleports, preserves initial segment velocity, extends bounded extrapolation to 150 ms, avoids per-frame hybrid follower reinitialization and leaves turning to retail during travel. Rare emergency teleports can still have visible consequences. No guarantee of zero visual deviation or 100% sync.

## In-game validation (no unit tests)

1. Two players, one Alien, straight corridor: compare physical location, turning, starting, stopping and gait on both PCs.
2. Reproduce sharp turns, sprint, stairs, elevator and large ping/jitter; verify correction is bounded and no old pose reappears.
3. Validate roar, reaction, scripted animation, paired kill of host and client, vent entry/traversal/exit; no direct pose commits while those actions own motion.
4. Try Android and other **admitted** NPC classes; check eligible native cycle and animation. Do not assume Facehugger support.
5. Trigger death, respawn, unloading/loading, world change, leave/join, host disconnect, PvP Alien possession. Ensure stale/previous-birth positions never apply.
6. Watch for collision clipping, excessive teleports, animation foot sliding, and TCP backlog.

The user builds and runs in-game validation; no automated tests were run or changed for this branch.
