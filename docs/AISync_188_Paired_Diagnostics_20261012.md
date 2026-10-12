# AISync 188 — analyse du kill Host du 12 octobre

## Archives reçues

`AIMP-Diagnostics-20261012-112711.zip` est le diagnostic Host de la partie
`b45ca4cd2f11404c87b710d52d448b7a`. Il contient Launcher 188, DLL 188,
IPC connecté, rôle AISync Host et génération monde 1.

`AIMP-Diagnostics-20261012-104252.zip` a été exporté sur le Client, selon
l'utilisateur. Son contenu couvre l'ancien problème IPC de 10:42 : Launcher 187,
DLL 188 refusée, aucun handshake validé. L'origine de l'archive avait été
incorrectement attribuée au Host dans la première analyse. Cette archive ne
couvre pas la partie de 11:27. Le Client actuel est confirmé en 188 par
l'utilisateur ; sa trace de la partie de 11:27 reste nécessaire.

## Ce que prouve le Host

- Une paire et une transition sont capturées, l'initialisation native réussit.
- La paire lie la source `B360341C/4C41EDAF` au joueur Host ; source `4E`,
  victime `4F`, outcome natif fatal de kind 1.
- Le callback victime observé avec l'identifiant `346045730` passe
  `victimEnded` à 1 et appelle la finition native de la source, qui passe
  `sourceFinished` de 0 à 1. Ces événements sont conservés dans le journal.
- `PlayerLifecycle` atteint la frontière de mort locale et supprime le GameOver
  solo ; la session et l'autorité Host persistent.
- À 11:27:11, le transport Host compte 928 états produits et 915 envoyés.
  Ces compteurs prouvent la production et l'envoi, pas l'application Client.
- Aucun défaut natif AISync n'est enregistré dans le dernier snapshot Host.

La disparition de l'animation et les deux personnages immobiles sont rapportées
par l'utilisateur côté Client. Les compteurs de réception, les identités locales,
l'admission de la paire et les callbacks Client manquent dans les archives
reçues. Leur cause ne peut pas être attribuée à la navigation, au remapping de la
victime ou à la libération du cycle à partir du seul Host.

## Correction de trace démontrée

Les six entrées `aisync_paired_graph` ajoutées en v188 écrivaient deux clés JSON
nommées `event` : le nom de la trace, puis l'identifiant numérique du callback.
Un lecteur JSON ordinaire conserve la dernière valeur ; ces entrées cessent
donc d'être reconnaissables sous leur nom de trace.

Le champ du callback est renommé `nativeEventId` dans `AISync.cpp`. La clé
`event` reste `aisync_paired_graph`. Pour analyser l'archive actuelle, les deux
occurrences ont été conservées lors de la lecture ; aucun callback n'a été
perdu dans cette analyse (`pairedGraphDropped=0`).

Cette correction répare le journal, pas le replay du kill. Aucun build ni test
impliquant une compilation n'est lancé, conformément à la demande utilisateur.
La modification de source est contrôlée par `git diff --check`.

## Étape nécessaire

Obtenir le diagnostic exporté sur la machine Client après le kill de cette
même partie. Comparer le cookie de session, `remoteReceived/nativeSent`,
`remoteQueued/applied`, l'identité source/victime, les refus d'admission,
`movementOwner`, les cycles locaux et les callbacks natifs avant toute
correction de comportement.
