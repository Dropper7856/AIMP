# PlayerLifecycle 201 — ouverture du menu pause

Launcher : `0.8.4.201`.
DLL : `0.11.4.201-playerlifecycle-pause-ui`.
Branche : `fix/aisync-native-execution`.

## Constats sur les archives

Client : `AIMP-Diagnostics-20261018-134055.zip`.
Host : `AIMP-Diagnostics-20261018-144106.zip`.

Le Client établit une connexion événement et envoie deux PAUSE_MENU_HELLO à
13:40:50. Il plante vers 13:40:53, avant toute demande Disconnect et avant un
retour au menu. Ces deux HELLO ne prouvent pas deux connexions Pause : une seule
connexion événement est enregistrée, et la version 200 envoyait depuis plusieurs
événements de cycle de vie. Le Host ouvre ensuite Pause sans planter.

Le thread fautif est 1832 ; le thread moteur identifié est 8928. Avec la base
chargée `0x510000`, la faute `0xEFDB75` correspond à `0xDEDB75` dans le PE fourni.
La chaîne cohérente rejoint D60B80, DD4180, DEDBC0 et DEDB50. Lors de la fusion
d'un bloc libre, `mov [ecx+4],edx` écrit à `0x5`, avec des liens de bloc valant
1 et 7. C'est une incohérence de métadonnées d'allocateur. Le rapport ne permet
pas d'identifier l'écriture antérieure responsable ni de nommer ce thread comme
un worker Socket.

À partir de stack[31], le fichier mélange des adresses de pile de deux plages
distinctes et contient un préfixe `sstack`. Cette partie n'est pas utilisée pour
attribuer un appelant. Le recorder permettait plusieurs ouvertures concurrentes
du même fichier avec CREATE_ALWAYS et FILE_SHARE_WRITE.

Preuves utilisées : archives, instructions du PE et fonctions correspondantes
dans AI.exe.c. SHA-256 du jeu fourni :
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.

## Corrections ciblées

La version 200 remplaçait `_backToFrontendItem` par un nouveau MenuItem ayant
un identifiant automatique. Elle retirait ainsi BACK_TO_FRONTEND, identifiant
retail 11, du mapping utilisé par le code natif.

La 201 conserve le même objet, sa page, ses bindings et son identifiant. Seuls
son nom et sa description deviennent Disconnect en multijoueur. Le handler
protégé est surchargé ; son chemin solo connu conserve l'appel retail.

Le pont Pause est différé jusqu'à `menu.initialized` et à la présence sur la
scène. Les callbacks réseau vérifient leur Socket d'origine et collectent des
lignes dans une file bornée. Ils ne modifient pas le menu et n'appellent pas
back_to_frontend. Une erreur signale seulement la fermeture à effectuer par
la prochaine frame UI.

La frame UI applique les réponses. Le clic sur Disconnect reste soumis à
l'accusé de nettoyage Launcher. Avant la première réponse d'état, un clic
attend la résolution du rôle au lieu de contourner la déconnexion. Après un
accusé positif, le Socket est fermé et ses listeners retirés ; le retour natif
est différé à la frame suivante. Les anciens callbacks ne peuvent pas utiliser
le Socket d'une nouvelle ouverture.

Les deux Timers et le polling HELLO chaque seconde ont été retirés. Une seule
demande d'état est envoyée par connexion/ouverture. Les délais et les reprises
bornées sont pilotés par la frame UI ; les buffers sont remis à zéro à chaque
fermeture. Le Socket propre au menu Pause reste présent : ce lot ne prétend pas
avoir démontré ou supprimé une concurrence interne de l'allocateur GFx.

Le recorder natif réserve désormais l'écriture au premier défaut non géré par
InterlockedCompareExchange. Les autres filtres suivent toujours leur chemin
normal mais ne peuvent plus écraser ou mélanger ce rapport. Cette correction
améliore le diagnostic ; elle ne répare pas l'allocateur.

## Vérification et limites

Seul PAUSEMENU.GFX a été régénéré avec FFDec et relu par export de son script.
Les balises graphiques restent identiques ; seul le script change. Les autres
GFX et UI.PAK ne sont pas modifiés par ce lot. Les informations statiques sont
conservées dans `analysis/playerlifecycle-201`.

Conformément à la consigne, aucun build Launcher/DLL, aucun test et aucun
lancement du jeu n'ont été effectués. Le lot corrige des défauts observables
dans le chemin introduit en 200, mais la disparition du crash et la cause
initiale de la corruption mémoire restent à confirmer. Le WorldManifest est
inchangé et n'a pas besoin d'être régénéré.
