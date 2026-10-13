# AISync v191 — refus de paquet et connexion IPC

La v191 corrige des chemins AISync qui pouvaient fermer la connexion IPC partagée
à la suite d'un refus de validation ou d'admission. Elle ajoute aussi la validation
des snapshots avant leur publication par le Host. Aucun nouveau hook moteur,
protocole réseau ou changement de WorldManifest n'est introduit.

## Ce que montre l'archive fournie

`AIMP-Diagnostics-20261013-080748.zip` appartient au Client, PID 8072, Launcher
`0.8.4.190`, DLL `0.11.4.190-aisync-rea-retail-execution`.

Le handshake IPC réussit à 08:06:43. La coupure du canal **UI** à 08:07:02 est
suivie d'une reconnexion à 08:07:08 pendant le chargement. À l'export de 08:07:48,
la DLL répond encore à la requête de diagnostics, le snapshot indique
`IpcConnected=true`, le moteur a traité 16043 callbacks et aucun événement natif
de déconnexion IPC n'est présent. AISync est installé, en rôle `FrozenClient=3`,
avec `worldGeneration=0` ; aucun état IA n'a encore été reçu ou appliqué. Le
démarrage réseau est donc encore en attente d'activation dans cette capture.

L'utilisateur confirme ensuite une déconnexion sur les deux machines. L'archive
ne contient pas cette coupure ni le diagnostic Host. Elle ne permet pas
d'attribuer cette coupure précise aux défauts corrigés ci-dessous. Il faut une
capture après la coupure pour établir cette attribution ou rechercher une autre
cause. Aucun crash, timeout ou défaut de hook n'est inventé à partir de cette
capture.

## Défauts démontrés dans le code

1. `DllIpcSession` validait les payloads `AISyncState` et `AISyncImpact` dans la
   boucle commune. Une `ProtocolException` de validation remontait au handler de
   connexion et déclarait tout l'IPC fermé. La v191 capture uniquement cette
   erreur de payload, refuse le paquet et conserve la lecture des messages
   suivants. Les abonnés AISync ne reçoivent jamais le paquet invalide.
2. `Valid(State)` côté Native n'exigeait pas `PoseValid` lorsque `Moving` était
   présent, contrairement au codec Launcher. `CapturePose` peut retirer ce flag
   après un échec de lecture sans retirer immédiatement `Moving`. La publication
   Host ne validait pas non plus l'ensemble de l'état avant de le placer dans sa
   file. La v191 impose la même règle et valide les deux chemins de publication,
   ordinaire et dormant. Un refus n'acquitte ni l'état ni une conséquence : le
   producteur pourra publier une observation valide ultérieure.
3. Les branches AISync de `LauncherIPC::HandleFrame` renvoyaient directement le
   résultat de `Configure`, `QueueRemote` et `QueueRemoteImpact`. Les refus de
   payload ou de capacité étaient alors convertis en
   `ipc_message_validation_failed`. La v191 traite ces refus comme des refus
   applicatifs AISync, les journalise et continue l'IPC. Les contrôles d'autorité,
   de rôle, de génération et de session sont conservés ; aucun résultat refusé
   n'est exécuté ou transformé en résultat autoritaire.

Les erreurs réelles de framing, de pipe ou de handshake restent des erreurs de
connexion. La v191 ne masque pas une fermeture du jeu et ne relance pas un
cerveau IA sur le Client. Elle ne garantit pas qu'un refus de paquet sera sans
effet sur la convergence AISync : une saturation ou un état invalide persistant
reste un défaut à diagnostiquer.

## Diagnostics ajoutés

Le Launcher journalise `Rejected AISYNC_STATE` / `Rejected AISYNC_IMPACT`, avec
séquence, taille et cause du validateur. La DLL journalise
`aisync_ipc_payload_rejected`, avec type de message, séquence, taille et compteur.
Ce dernier journal est limité aux quatre premiers refus puis à chaque 128e refus.

Les snapshots AISync ajoutent `invalidOutgoing`, `lastInvalidOutgoingFlags`,
`lastInvalidOutgoingEntityA` et `lastInvalidOutgoingEntityB`. Ils permettent
d'identifier un producteur refusé sans dépendre d'un paquet envoyé au Launcher.

## Versions et vérification

- Launcher : `0.8.4.191`.
- DLL attendue et Native : `0.11.4.191-aisync-ipc-isolation`.
- Protocole AISync d'état v11 / 288 octets natifs inchangé.
- Aucune régénération du WorldManifest nécessaire.
- Aucun build, test compilé ou lancement de `AI.exe` effectué.

Le test source `AISync rejection preserves IPC` envoie, après handshake, un état
tronqué, un état sans pose et un impact invalide, puis un état et un impact
valides. Il vérifie que seuls les deux derniers arrivent aux abonnés, que les
trois refus sont journalisés et que la requête de capacités et une commande
continuent à fonctionner sans fermeture de connexion. Ce test est ajouté mais
**non exécuté**. La revue du diff, la concordance des versions et
`git diff --check` constituent les vérifications statiques de cette révision.
