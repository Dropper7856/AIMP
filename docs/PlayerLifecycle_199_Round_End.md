# PlayerLifecycle 199 — fin de manche et retour au lobby

Launcher : `0.8.4.199`. DLL : `0.11.4.199-playerlifecycle-round-end`.

Rien n'a été compilé ni testé, à la demande de l'utilisateur. Seuls les deux
GFX ont été recompilés avec FFDec : ce sont des ressources livrées.

## Comportement

1. Chaque joueur meurt normalement. Le Game Over solo reste supprimé, comme en
   MVP1.
2. Quand **tous** les joueurs de la manche sont morts, le Host déclare la fin de
   manche.
3. Sur chaque machine, le HUD affiche « FIN DE LA PARTIE / Tous les joueurs sont
   morts. » et un compte à rebours « Retour au lobby dans N s » (5 s).
4. À la fin du compte à rebours, chaque jeu quitte le niveau par le chemin retail
   « Retour au menu principal ».
5. Le menu principal rouvre directement la page du lobby (Host ou Client). La
   connexion lobby n'a jamais été coupée. Les Clients restent prêts, donc le Host
   peut relancer immédiatement.

## Chaîne complète

```
DLL PlayerLifecycle (mort locale supprimée)
  -> LocalDeathSequence++                        PlayerLifecycle.cpp
  -> IPC PlayerLifecycleState (27)              LauncherIPC::PublishPlayerLifecycleTraffic
Launcher
  -> Host : LobbyHost.ReportHostPlayerDeathAsync
  -> Client : LobbyClient.SendPlayerDeathAsync -> PlayerDeath (122) -> Host
Host LobbyHost
  -> _deadPeers ; Host + tous les Clients morts -> état "Lobby"
  -> RoundEnded (123) diffusé + snapshot Lobby
Chaque Launcher (MultiplayerSessionCoordinator.RoundLifecycle.cs)
  -> bridge ROUND_END|title|message|seconds -> PICKUPOVERLAY
  -> 5 s -> fermeture du World bus, reset de l'état de manche
  -> commande native RETURN_TO_FRONTEND
DLL GameCommandExecutor::ExecuteReturnToFrontend
  -> FUN_00793880(G, 1) sur le thread moteur
Menu principal rechargé -> HELLO -> CONFIG + LOBBY_STATE + LOBBY_RETURN|role
  -> NEWFRONTENDMENU ouvre la page Lobby
```

## Preuve du retour au menu (binaire certifié)

- La table des callbacks UI `FUN_0084DBE0` enregistre `back_to_frontend` vers le
  handler `00806F00` et `request_back_to_frontend` vers `00849830`.
- `00849830` → `00847FD0` n'ouvre que la boîte de confirmation.
- `00806F00` → `00805620` : si le menu ne bloque pas, il appelle
  `FUN_00793880([[GlobalStateRoot]+48], 1)`.
- `FUN_00793880` : `ECX = G`, un argument, `RET 4`. Il prend lui-même la section
  critique `G+1DC` et ne fait rien si `NEW_FRONTEND_MENU` est déjà chargé.
  Sinon il demande le niveau `frontend`, fixe `G+1F5=1`, `G+1F8=2`, puis appelle
  `FUN_00793780`, qui passe `G+8=7` et `G+14=0`.
- Nouveau symbole `GameplayReturnToFrontend`, RVA `0x00393880`, signature de 32
  octets (opérande SEH masqué). La révision du profil passe de 15 à 16.
- La commande refuse d'agir si le mode gameplay vaut 0 ou 3 (frontend/inactif),
  et elle est protégée par SEH.

## Règles de fin de manche (Host)

- Une mort n'est comptée que pour la génération active (`LevelGeneration`) et
  une seule fois par joueur.
- La manche se termine quand le Host et chaque Client connecté sont morts.
- Si un Client se déconnecte, la manche est réévaluée. Si tous les joueurs
  restants sont morts, elle se termine.
- La DLL ne republie jamais une mort survenue hors session active.

## Fichiers

| Zone | Fichiers |
| --- | --- |
| Protocole | `MessageType.cs` (27, 122, 123), `ProtocolModels.cs`, `ModelValidation.cs`, `LobbyHost.cs`, `LobbyClient.cs`, `ExperimentalProtocolModels.cs` (`RETURN_TO_FRONTEND`) |
| Launcher | `DllIpcSession.cs`, `NativeRuntimeController.cs`, `MultiplayerSessionCoordinator.cs` (partial, abonnements, HELLO, profil `round-end-v1`), nouveau `MultiplayerSessionCoordinator.RoundLifecycle.cs`, `ProductInfo.cs` |
| DLL | `Protocol.h/.cpp`, `PlayerLifecycle.h/.cpp`, `LauncherIPC.h/.cpp`, `GameCommandExecutor.h/.cpp`, `ExperimentalProtocol.cpp`, `BuildProfile.h/.cpp`, `ProductVersion.h` |
| UI | `Resources/UI/PICKUPOVERLAY.GFX`, `Resources/UI/NEWFRONTENDMENU.GFX` ; sources AS modifiées dans `Resources/UI/Source/` |
| Tests (non exécutés) | `ProtocolTests.cs` (commande sans argument), `NativeTests.cpp` (id 27, profil 16 / 108 symboles, symbole RVA) |

## GFX

Les scripts ont été exportés avec FFDec depuis les GFX du Launcher. Seules les
classes modifiées ont été réimportées (`-importScript`).

- `PickupOverlay` : handler `ROUND_END` qui affiche un calque plein écran avec
  la police du message scripté, un `Timer` de 1 s et l'accusé `ROUND_END_ACK`.
- `frontend.MultiplayerBridge` : message `LOBBY_RETURN` → `onLobbyReturn(role)`.
- `frontend.NewFrontendMenu` : attente de l'état MENU (passe l'écran SPLASH après
  30 frames), puis `menu.setCurrentPage(page Lobby)`.

`NEWFRONTENDMENU.GFX` contenait déjà une balise `PlaceObject2` illisible pour
FFDec (ID 26). Elle se trouve dans la partie finale du fichier, conservée
octet pour octet.

## À vérifier en jeu

1. Host + Client : faire mourir les deux. Message et compte à rebours sur les
   deux écrans, retour au menu, page Lobby ouverte, relance possible.
2. Ordre inverse (Client puis Host, et Host puis Client).
3. Un seul mort : la partie continue, aucun message.
4. Déconnexion du Client pendant que le Host est mort : fin de manche côté Host.
5. Logs Launcher `PlayerLifecycle` : « Local player death », « Round N ended »,
   et aucun « Return to frontend was rejected ».
6. Si le message n'apparaît pas alors que `ROUND_END` est envoyé, chercher
   `ROUND_END_ACK` / `round_end` dans les logs du bridge.
