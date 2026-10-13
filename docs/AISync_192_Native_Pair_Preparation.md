# AISync v192 — navigation, préparation de paire et possession perdue

Cette révision corrige l'exécution Client observée dans le test v191 : l'Alien
prend du retard pendant sa course, est réaligné lors de l'attaque, puis sa source
`4E` quitte la paire avant la création de la victime `4F`. La possession conservée
par AISync bloque ensuite la suite de la réplication.

Le patch utilise l'autorisation et la définition déjà résolues par le Host pour
rejouer une transition native de préparation. Il conserve la navigation locale
et réplique l'allure qui était réglée séparément par les actions natives du Host.
Il ne réactive pas de cerveau Client et ne transforme pas la mort en simple
animation ou en règle `HP=0 → graph terminé`.

## Diagnostic des deux archives

Les rôles sont déterminés par les logs et les identités de session :

- **Host** : `AIMP-Diagnostics-20261013-100146.zip`, PID 9936.
- **Client** : `AIMP-Diagnostics-20261013-090155.zip`, PID 9092.
- Les deux utilisent Launcher/DLL v191, IPC connecté et AISync actif.
- Cookie de session commun : `5756417985755971783`.
- Identité de l'IA : `3009426460 / 1279389103`.

Les heures locales diffèrent entre les postes. Les observations natives sont
aussi journalisées par lots : `sampleTick` est l'instant de capture, le timestamp
JSON peut être l'instant de vidage. Les ticks sont propres à chaque machine et
ne doivent pas être directement soustraits entre Host et Client.

| Observation | Preuve dans les logs natifs |
| --- | --- |
| Retard de locomotion avant l'attaque | Client séquence 398 : source locale `(-22.636, -27.122)` en X/Z, source autoritaire `(-34.772, -22.529)`, soit environ **13 m** d'écart horizontal. |
| Réalignement de la paire | Client séquence 400 : source déplacée près de la position d'admission, placement encore en attente. |
| Source créée, victime absente | Client séquence 401, tick `4434296` : source `4E`, étape `0`, victime en Idle `1`, HP 1000. |
| Source perdue et possession conservée | Client séquence 402, tick `4435296` : source `EAD858 / 1`, victime en `38`, `ownsPair=true`. |
| Blocage durable | Client séquence 434 : état traité toujours révision 207/paire 1, `pairedInitializeSucceeded=1`, `pairedVictimTransitions=0`, `nativeOutcomeApplied=0`, aucune trace de graph paired Client. |
| Le Host continue son arbitrage | Host séquences 517–518 : paire 3 dirigée contre le Client vivant, puis conséquence native. Le Client reste sur l'ancienne paire 1. |

Le freeze n'est donc pas ici un défaut de libération après une véritable fin de
graph victime : **aucun `4F` n'a démarré sur le Client**. Le code attendait
indéfiniment qu'une conséquence non consommée soit admise, sans revérifier que
le cycle source nécessaire existait encore.

Le retour vers `EAD858 / 1` correspond au chemin retail de reset NPC
`0065BDD0 → 00656430`. La capture v191 ne journalise pas l'appelant exact ni les
flags au moment du reset. Elle ne permet pas de départager toutes les raisons
natives d'interruption. Le patch corrige la frontière d'exécution manquante et
le verrou durable démontré ; il ne prétend pas avoir observé une stack de ce reset.

## Préparation native autoritaire

Le Host a déjà exécuté `00662670` lorsqu'il publie `OutcomePrepared` avec
`OutcomeVictimAdmitted`, la définition choisie, la géométrie et les conséquences.
Le Client v191 recréait la source mais attendait encore une progression locale
de l'étape 0 avant d'atteindre ce producteur. Les traces montrent que cette
progression n'aboutissait pas avant la disparition de la source.

La v192 rejoue la transition d'exécution retail correspondante :

- `006638D0` appelle le producteur existant `00662670`, règle ses flags de mode
  puis entre dans l'étape 2 ;
- `00663900` conserve son prédicat natif de mode, appelle le même producteur et
  entre dans l'étape 5 pour le chemin alternatif.

Le rejeu exige un Client stable, la paire autoritaire exacte, les incarnations
publiées source/victime encore vivantes, le cycle source `4E / EAE5D0` courant,
son owner, sa victime et son cache de durée de vie corrects, aucun flag de fin,
aucun worker en admission et aucune conséquence déjà consommée. Il est limité
aux étapes de préparation 0/1 ou 4 ; les étapes actives ou terminées ne sont
pas rejouées.

Ces fonctions passent par le detour de préparation existant, son
`ClaimOutcome` et le véritable message victime produit par le moteur. Elles
réutilisent la définition Host ; aucun clip, marker, packet synthétique ou cible
locale n'est choisi. La transition remplace une attente locale d'admission par
une exécution déjà autorisée, sans raccourcir la durée du graph victime.

Les deux ABI sont `ECX=this`, sans argument de pile, avec `RET` simple. Les
calls, écritures d'étape et retours ont été vérifiés dans le PE et sont également
contrôlés à l'installation. Les pseudocodes REA conservés de `006638D0` et
`00663900` appartiennent au lot
`ev_c2d87787ffe26d7ebe9ddcfdcad2449019831428406800660e56905e5b15e68d`.

## Navigation : allure et arrêt sont des décisions distinctes du chemin

Les producteurs retail `00684900` et `00683E80` écrivent l'allure demandée dans
le mouvement Character, à `+194`, puis réinitialisent `+190`. Cette écriture est
séparée de la création/soumission de la requête Move. Le Client supprime les
actions et le Behaviour Tree concurrents, ce qui retire aussi ces écrivains.
La requête de navigation seule ne les remplace pas.

AISync applique désormais l'allure capturée sur le Host au mouvement local
lorsque la navigation possède l'acteur. La route, le steering, la vitesse
physique et l'allure effective restent calculés par le moteur. Les champs privés
de vitesse, les transforms et les animations ne sont pas forcés. L'écriture
est faite lors d'un changement de valeur.

Le contrat a été confronté au décompilé fourni et aux instructions du même PE :
`00684C28` et `00684041` contiennent les écritures `+194` puis `+190=0`.
Ce contrôle ne prouve pas que l'allure explique à elle seule tout l'écart de
13 m ; le prochain essai permettra de mesurer le mouvement obtenu.

Une requête Move peut également rester publiée pendant un arrêt retail `3B`.
La fin du cycle **local** de stop ne constitue pas une autorisation de repartir.
La v192 suspend la requête tant que le Host rapporte ce stop certifié sans
mouvement. Une nouvelle exécution Host permet de reprendre le même objectif.
Cela conserve la distinction entre l'acquittement d'une transition et la
possession du mouvement.

## Récupération d'une admission perdue

La possession est revérifiée avant le verrou des snapshots et dans la boucle
d'exécution. Les échecs de lecture restent une incertitude ; un worker actif ou
un véritable `4F` courant interdit la récupération destructive.

Si les cycles nécessaires ont disparu avant consommation, AISync retire leurs
acquittements locaux et permet au maximum **deux réessais** de la même paire,
avec le délai d'admission existant de 250 ms. Après épuisement, cette paire est
retirée de la présentation et les nouvelles décisions Host peuvent avancer.
Si la conséquence était déjà consommée, elle n'est jamais rejouée : la possession
de cycles disparus est simplement retirée. Aucun dégât ou décès n'est fabriqué
pour compenser un échec de présentation.

Cette récupération concerne une paire interrompue ou jamais admise. Elle ne
déclare pas une animation terminée sur la seule base des HP ou d'un timeout.
Le mécanisme de fin locale native ajouté précédemment reste utilisé pour les
victimes qui possèdent réellement leur graph.

## Vérification et prochain essai

- Branche : `fix/aisync-native-execution`.
- Launcher : `0.8.4.192`.
- DLL attendue : `0.11.4.192-aisync-native-pair-preparation`.
- Protocole d'état v11/288 octets inchangé ; aucun WorldManifest à régénérer.
- Aucun build, test compilé ou lancement du jeu effectué.
- Audit PE : **40 contrôles statiques sans échec**, SHA-256 exécutable
  `00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
- Tests sources ajoutés : choix des seules transitions de préparation,
  protection du worker/graph victime, non-rejeu d'une conséquence consommée,
  arrêt Host conservé malgré un objectif encore publié. Ils sont non exécutés.

Les nouveaux diagnostics incluent `gaitApplied`, `authorityMoveType`,
`requestedMoveType`, `effectiveMoveType`, `cycleFlags`, `pairedPreparedReplays`,
`pairedAdmissionRetries` et `pairedAdmissionAbandoned`. Les phases de présentation
7 et 8 marquent respectivement le rejeu de préparation et une admission perdue.

Le test à refaire est celui fourni : observer depuis le Client la course puis
la mort du Host, vérifier l'acquisition du `4F`, sa fin native et la poursuite du
Client vivant. Une préparation normale doit faire progresser les compteurs de
transition victime et de conséquence, sans réessais ou abandon permanent. La
fluidité et le succès des nouvelles transitions restent à confirmer en partie.
