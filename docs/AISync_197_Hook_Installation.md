# AISync 197 — correction du refus d'installation

Branche : `fix/aisync-native-execution`.
Launcher : `0.8.4.197`. DLL : `0.11.4.197-aisync-hook-installation`.

## Diagnostic fourni

Archive Client : `AIMP-Diagnostics-20261016-194914.zip`.
Elle confirme les versions 196, une connexion IPC établie et un jeu chargé à
`0x140000`.

À 19:47:57, `aisync_v2_install` échoue avec
`invalid or non-certified hook request`. Le Launcher désactive uniquement AISync
et conserve l'IPC des autres modules.

WorldSync charge ensuite son manifeste compatible, accepte la récupération
Host `checkpoint=0 hostThrough=0` et passe à `WorldBus=Active` à 19:48:41.
Cette archive ne montre donc pas un refus d'activation WorldSync. Elle ne prouve
pas à elle seule que chaque porte a été répliquée : aucune mutation mondiale
n'est reçue dans la fenêtre enregistrée.

## Cause et correction

Les nouveaux hooks `aisync_host_resolved_shot`,
`aisync_directed_world_goal_start` et `aisync_directed_world_goal_update` de la
version 196 renseignaient `publishedTrampolineOut`, mais omettaient
`trampolineOut`. Le contrat de `HookManager::Install` exige les deux pour ces
demandes : la sortie classique assure le suivi et la libération du trampoline,
la sortie atomique publie ce dernier aux detours avant reprise des threads.

Le hook de tir est installé en premier parmi ces trois demandes. Son refus
entraîne le rollback du lot AISync. Les deux demandes de navigation auraient
ensuite échoué pour la même raison.

Les trois sorties classiques ont été ajoutées, en conservant la publication
atomique et toutes les gardes existantes. Leurs erreurs d'installation indiquent
désormais le nom du hook concerné. Aucun assouplissement de certification ni
changement du contrat partagé de HookManager n'est nécessaire.

Le protocole AISync reste en version 12, snapshot de 640 octets. Les sources
WorldSync et le WorldManifest sont inchangés ; aucune régénération de manifeste
n'est nécessaire. Mettre à jour ensemble le Launcher et la DLL pour respecter
leur contrôle de version.

## Vérification reproductible hors jeu

Le nouveau test `AIMP.AISyncHookTests` charge les sections du PE comme données
privées, applique ses relocations HIGHLOW et appelle les véritables routines
d'installation santé/combat/navigation/animation avec le vrai HookManager.
Il exige une adresse différente de la base préférée du PE. Il vérifie aussi le
refus d'une ABI altérée, les huit patches, leur restauration et la libération
des trampolines après démonstration de leur quiescence.

Il ne résout aucun import, ne lance aucun entry point ou callback TLS et
n'appelle aucune instruction du jeu. Ses patches concernent uniquement sa
copie privée en mémoire ; le fichier AI.exe n'est pas modifié.

Le même test, lié à la bibliothèque isolée de la version 196, reproduit :

```text
FAIL: Combat installation: invalid or non-certified hook request
```

Avec la bibliothèque 197 :

```text
PASS: eight AISync hooks installed and rolled back on rebased PE data; no game code executed.
```

Les compilations Release isolées DLL x86 et Launcher réussissent. Les deux
tests CTest AISync passent, ainsi que les trois tests managés ciblés frames,
codec AISync et maintien de l'IPC après rejet AISync. Les 291 contrôles statiques
PE restent valides. Sorties : `analysis/aisync-validation-197`.

Le jeu n'a pas été exécuté et aucun déploiement n'est effectué. Ce test couvre
le refus d'installation identifié, pas le rendu ou le gameplay complet AISync.
