# 203 — accusé de retour au lobby et pause de navigation de l'Alien Client

Launcher : `0.8.4.203`. DLL : `0.11.4.203-round-return-ack-nav-pause`.

Rien n'a été compilé ni testé. Seul `PICKUPOVERLAY.GFX` a été recompilé avec FFDec.

## 1. Blocage sur « Returning to lobby »

Diagnostics du 18 octobre (v202), Host et Client : après le compte à rebours, le
HUD envoyait `ROUND_RETURN_READY` puis fermait aussitôt sa socket. Le Launcher ne
recevait jamais ce dernier message (le `close()` de GFx abandonne l'écriture en
cours). La barrière du Launcher attendait donc en vain, sans jamais demander la
sortie native : le jeu restait sur l'écran de fin, sans freeze, sur les deux
machines.

Correctif (accusé de réception avant fermeture) :

1. Le HUD envoie `ROUND_RETURN_READY` et **ne ferme plus sa socket**.
2. Le Launcher valide l'identité (session, génération, jeton) et répond
   `ROUND_RETURN_CLOSE` sur cette même connexion
   (`ScaleformBridgeEvent.Reply`).
3. Le HUD consomme la réponse, et ne ferme sa socket qu'à la frame suivante.
4. Le Launcher n'émet `RETURN_TO_FRONTEND` qu'après avoir écrit l'accusé **et**
   constaté la fermeture de cette connexion.
5. Si l'accusé n'arrive pas en 10 s, le HUD affiche « Unable to return to lobby.
   Use Disconnect in Pause. » et se ferme de lui-même ; le menu pause reste
   utilisable.

Un reset TCP envoyé par GFx à la fermeture est traité comme une fermeture
normale du pair, sauf si la connexion a été remplacée localement.

## 2. Alien Client en avance sur le Host

Les traces montrent que l'Alien Client continue vers l'objectif natif alors que
la locomotion du Host est retombée à zéro, et prend jusqu'à six mètres d'avance.

- `HostPausesPointNavigation` : un objectif ponctuel vivant n'autorise plus le
  déplacement quand l'exécuteur Move du Host (`00EABF80`, état `38`) est à
  l'arrêt ou sans intensité. Les cycles de rotation, de posture et de
  mouvement avec root motion gardent leur propre contrôle.
- Dans ce cas, la navigation Client est annulée et l'Alien est tenu en Idle
  jusqu'à la prochaine décision du Host. L'objectif natif reste publié.
- Le rattrapage par navigation locale est maintenant possible avec un objectif
  Host actif, mais seulement pour un proxy **en retard** le long de la direction
  de déplacement du Host, au-delà de 2,5 m et après 500 ms. Un proxy déjà en
  avance ne fait jamais demi-tour pour rejoindre une ancienne pose.
- Un changement de requête Host annule le rattrapage en cours.
- Nouveau compteur `navigationHostPauses` dans `aisync_v2_snapshot`.

## À vérifier en jeu

1. Fin de manche : après 5 s le jeu revient bien au menu et rouvre le lobby, sur
   le Host et le Client. Logs `PlayerLifecycle` : « HUD ready; acknowledging »
   puis « acknowledged HUD connection closed », puis la commande
   `RETURN_TO_FRONTEND`.
2. Déplacements : comparer la position de l'Alien sur les deux machines pendant
   les arrêts du Host. `navigationHostPauses` doit augmenter,
   `navigationConvergenceApplied` doit rester faible.
3. Si l'Alien Client reste figé alors que le Host marche, c'est que la pause est
   trop large : fournir les deux diagnostics.

Pas de changement de protocole réseau. Aucune régénération du WorldManifest.
