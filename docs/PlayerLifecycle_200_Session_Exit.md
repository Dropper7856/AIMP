# PlayerLifecycle 200 — retour au lobby et déconnexion

Branche : `fix/aisync-native-execution`.
Launcher : `0.8.4.200`.
DLL : `0.11.4.200-playerlifecycle-session-exit`.

Ce lot termine les changements interrompus : nettoyage au retour au menu,
textes anglais, bouton multijoueur Disconnect et notification des départs.
La fin de manche conserve la connexion du lobby. Disconnect ferme la session
réseau et revient au menu principal.

## Diagnostic du crash

Le diagnostic Client `AIMP-Diagnostics-20261018-123315.zip` montre la fin de
manche à 12:33:01, la demande RETURN_TO_FRONTEND à 12:33:06 et un crash quatre
secondes plus tard. Le Host ne présente pas ce crash dans son archive
`AIMP-Diagnostics-20261018-133320.zip`.

Adresse de faute `0x00EFD9C0`, base chargée `0x00510000` : l'adresse préférée
est `0x00DED9C0`, dans l'allocateur `FUN_00DED990`. L'instruction
`mov edx,[eax+4]` lit une tête de liste nulle alors que le bitmap de la classe
d'allocation indique une entrée. La pile rejoint les allocations du frontend.
Ce constat révèle une incohérence d'allocateur ; il ne désigne pas l'écriture
qui l'a provoquée. La suppression du crash n'est pas établie sans nouvel essai.

Le wrapper de retour natif conserve la bonne ABI : le callback retail
`0x00805620` charge `G=[[GlobalStateRoot]+48]`, pousse `1`, puis appelle
`0x00793880` en thiscall. Le callee finit par RET 4. Le chemin
`0x00793780` fixe immédiatement le mode `G+14=0` et l'état `G+8=7`.
Ces observations viennent des instructions du PE et du décompilé fourni,
SHA-256 `00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
La demande de décompilation REA supplémentaire a expiré ; sa session a été
fermée. Elle ne constitue pas une preuve de décompilation réussie.

## Nettoyage natif

Un défaut de lifetime concret est corrigé : le contrôleur de mouvement gardait
des adresses Character/cycle après leur destruction et la fin du niveau.

- `ForgetCharacter` invalide le mailbox avant la destruction complète native.
- `ForgetAll` invalide les mailboxes avant le retour au frontend, sans envoyer
  un Stop natif à des objets en cours de destruction.
- La racine gameplay provient du symbole GlobalStateRoot déjà résolu par le
  profil. Aucun pointeur de gestionnaire de niveau n'est conservé.
- Armement, pumps, yaw, transitions et callbacks de mouvement refusent les
  écritures lorsque le contexte est illisible, le mode vaut 0/3 ou l'état vaut
  7. Une commande tardive ne peut donc pas réarmer un mailbox après sa purge.
- Les hooks Move/Idle rendent la main au retail pendant le déchargement.

La même barrière couvre le retour retail du menu pause, qui ne passe pas par
la commande AIMP RETURN_TO_FRONTEND. Le gameplay suivant retrouve son contexte
normal ; aucun verrou permanent ni restauration forcée de mémoire n'est ajouté.

## Déconnexion et interface

Le menu pause utilise le port événement du bridge, avec réponse sur son propre
socket : il ne remplace pas le canal commande du HUD/menu principal.

En partie multijoueur, Disconnect remplace le retour au menu retail. Le GFX
envoie PAUSE_MENU_DISCONNECT avec un identifiant de demande. Le Launcher arrête
les producteurs managés, détache le réseau, ferme l'autorité WorldSync et
dispose la connexion. L'accusé PAUSE_MENU_DISCONNECT_READY autorise ensuite le
GFX à appeler `back_to_frontend`. Le timer de 15 s affiche un échec réessayable,
sans forcer une sortie pendant un nettoyage non confirmé.

La fermeture WorldSync est traitée directement par l'IPC. La transition AISync
est soumise à son prochain pump moteur : l'accusé n'affirme pas que toutes les
transitions natives ont déjà été exécutées pendant Pause. La barrière de
déchargement native protège les adresses dans cet intervalle.

Les joueurs restants reçoivent PopupMessage avec `X left the game`. Si le Host
quitte, les Clients ferment leur session, demandent ce popup et prévoient le
retour au menu après cinq secondes. La commande native dépend de la reprise
du dispatcher si le jeu Client est lui-même en pause.

Les callbacks de déconnexion/snapshot vérifient leur source et leur session.
Les comptes à rebours revalident session/génération ou version de fermeture,
afin qu'un ancien départ ne renvoie pas une nouvelle partie au frontend.

Textes de fin de manche : GAME OVER, All players are dead., Returning to lobby
in 5 s. Le HUD retire son timer/socket AIMP lors du déchargement. Le menu pause
ferme aussi son socket et ses timers quand son movie est retiré.

## Ressources et limites de vérification

PAUSEMENU.GFX rejoint les quatre ressources injectées par le Launcher. Les
sources ActionScript et les trois GFX modifiés sont inclus. Le fichier installé
UI.PAK n'a pas été modifié par ce travail.

Les ressources GFX ont été régénérées par FFDec puis relues statiquement. Seules
leurs balises de scripts diffèrent : les balises graphiques restent identiques.
L'erreur de lecture FFDec de la balise PlaceObject2 du menu principal existe
dans la ressource précédente ; ses données non interprétées sont conservées.
L'inventaire et les empreintes se trouvent dans
`analysis/playerlifecycle-200/ui-resource-inspection.json`.

Conformément à la consigne, aucun build Launcher/DLL, aucun test et aucun
lancement du jeu n'ont été effectués. Les attentes du test existant de catalogue
UI ont simplement été mises à jour pour les cinq fichiers. La revue est
statique : compilation et disparition effective du crash restent à confirmer.
Le WorldManifest ne change pas et ne doit pas être régénéré pour ce lot.
