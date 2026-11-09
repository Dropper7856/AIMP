# Campaign synchronization — native experiment 488

## Evidence from both 2026-11-09 campaign diagnostics

- TCP/session and WorldSync stayed active. This was not a network disconnect.
- Both machines reported an accepted `SpawnCharacter` request with `spawnKey=2`,
  but no native `CharacterCreate` completion and a permanently `Pending` tracker.
- Manifest preparation enumerated campaign levels with `includeCampaign:true`,
  while the E_RIPLEY/DisplayModel importer called `RipleyCharacterInstaller.Install`
  with its default `includeCampaign:false`. Campaign levels were missing the
  character asset closure. Recipe 15 now imports campaign levels too and forces
  a new preparation, **with the game shut down**.
- MissionSync had no captured/applicable objective commits and recorded 657/748
  unsafe payload-bearing outputs. The compiler now marks a campaign output as
  payload-tolerant **only when every authored destination has already been
  proven to be a data-free control input**. Arbitrary typed CATHODE payloads
  still stay local. This is a targeted hypothesis to verify on a terminal,
  not a blanket payload bypass.

## Cinematic input authority

- The native campaign role comes from MissionSync's configured Host/Client role.
- The existing validated AlienPvPInputIsolation PE-IAT hook is reused, **not**
  installed a second time. Only clients have local VK_RETURN filtered, and only
  while their current camera differs from the certified retail follow/head
  cameras, with a short heartbeat lease from the launcher.
- The launcher polls the native cinematic state at 5 Hz. Holding Enter on the
  Host for approximately 550 ms produces one Host-only network
  `CinematicSkipCommitted` event with session/generation/monotonic sequence.
  No client packet is authorized to propose a skip.
- A client pressing Enter sees a status notification and an attempted in-game
  popup: `Seul l'hôte peut passer la cinématique.`
- An accepted Host skip temporarily synthesizes a Return hold through the
  game's own input polling/message queue (1650 ms, with a release message).
  **No unverified retail cutscene-skip function is called.**
- If the IAT hook, camera proof, focus, or session heartbeat is unavailable,
  cinematic input redirection fails closed. Pre-rendered movies and unusual
  camera modes may **not** satisfy this camera predicate; coverage is not yet
  confirmed. The native module does not promise a universal cutscene detector.

## Validation checklist

1. Pull/build v488 and rerun Launcher game preparation with AI.exe stopped.
2. Compare `SpawnCharacter` → `Ready` / nonzero `CharacterBase` on both peers.
3. Change an authored objective at a campaign terminal and compare MissionSync
   `captured`, Host `committed`, client `applied`, plus actual UI objective.
4. During a camera-controlled cinematic, hold Enter on client: it must not skip
   and should display the Host-only message.
5. Hold Enter on Host: seek `CinematicSync: Host skip committed` and
   `Host cinematic skip applied` in client logs, and confirm both skip visually.
6. Test pre-rendered intro movies separately. If the camera predicate fails,
   collect both fresh diagnostics before extending native detection.
7. Disconnect client during a cinematic: the input firewall must expire after
   the heartbeat rather than trapping the Return key.

No Windows x86 native build or live game replay was performed in the edit environment.
