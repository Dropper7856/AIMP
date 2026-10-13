# AISync v190 — exécution retail vérifiée avec REA

Cette révision corrige plusieurs défauts concrets de l'exécution AISync : une
classification incorrecte des victimes humaines distantes pendant une paire,
la perte d'une notification locale de fin, une possession de mouvement conservée
après la fin d'un cycle et des reprises de navigation inutilement abandonnées.
Elle conserve le cerveau IA sur le Host et utilise le moteur du Client pour
exécuter les objectifs et les interactions reçus.

La validation est **statique**. Aucun build AIMP, test compilé ou lancement du jeu
n'a été effectué, conformément à la demande. La révision ne certifie pas encore
la fluidité en partie ni la prise en charge complète de toutes les IA.

## Périmètre et preuves

- Branche : `fix/aisync-native-execution`, à partir de v189.
- Launcher : `0.8.4.190`.
- DLL : `0.11.4.190-aisync-rea-retail-execution`.
- AISync conserve le protocole d'état v11, 288 octets natifs et 304 sur le réseau.
- Aucun changement à PlayerSync, WorldSync ou PlayerLifecycle.
- **Aucune régénération du WorldManifest n'est nécessaire pour cette révision.**

Exécutable étudié : `AI.exe`, PE x86, base préférée `0x00400000`, SHA-256
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
Les adresses ci-dessous sont les adresses virtuelles préférées ; le code utilise
leurs RVA et la base effective du module.

REA 6.4.0 et Ghidra 12.1.2 ont produit 73 requêtes ciblées : 67 résultats réussis,
66 procédures canoniques distinctes. Cinq entrées n'ont pas été reconnues comme
fonctions par Ghidra ; une requête comportait une adresse mal saisie, corrigée
ensuite. Des sélecteurs peuvent désigner l'intérieur d'une fonction : l'index
conserve séparément le sélecteur et l'entrée canonique. Les signatures décompilées
ne suffisent pas à certifier les ABI ; les limites de hooks et les conventions
d'appel ont aussi été confrontées aux instructions x86.

Les résultats locaux sont conservés dans `analysis/rea-aisync/index.json`, les
pseudocodes et les réponses par lot, avec l'export `evidence.json`. Ces fichiers
générés sont ignorés par Git. Les identifiants de preuve utiles sont :

| Sujet | Preuve REA |
| --- | --- |
| Création, soumission et mise à jour des requêtes | `ev_4f16fef7df78a647594de61f98141f63c4936140b6955b55fde59a1bacf38373` |
| Paired et navigation | `ev_d7ade1fd52f876e8436a04cb36e063804701611487967298100438d71d226b03` |
| Scheduler physique | `ev_1c21cd912c06c5adde1e8923d72ce2367feb10ff33bb534daf842bfdfdd473f3` |
| Exécution de navigation | `ev_2c8f7c736c0a05590b9fb76c4b00f4c8a8f3db50e352be227d779ab059daee22` |
| Mouvement et cibles | `ev_8bb2f625ae51f5576c0096f8afe7e5ce813f04e8ab5ec0d2c550af67402cd2fd` |
| Sélection | `ev_aab0bc659f5d3bcfc32d196c8fafc6a8f56eb3310441e5a1d7d5a3c186bb7e4e` |
| Arbitrage | `ev_9ca5b8befaa2b62a55516921b6a99e27a2bf4b9f130e9a3a8294003127131e3f` |
| Classes et promotion | `ev_6977e8c3586d7b350705c9dd889185609b3af15dc9e843f6a54274145b5d195d` |

## Paired : classification pendant l'exécution

Dans `FUN_00663E00`, le cycle source `4E` vérifie la victime à `00663E51`.
Une victime retail joueur rejoint directement le dispatcher de phases à
`00663EA9`. Une victime NPC passe par un contrôle du contrôleur IA de la source
(`Character+4234`, phase `+74`) qui peut modifier son mode et terminer la paire.
Une représentation humaine distante est construite comme un NPC. Ce passage
dépend donc, sur le Client, d'un état du cerveau IA qui n'est pas répliqué et dont
l'exécution est volontairement désactivée.

Le nouveau hook adapte cette seule classification à la participation multijoueur.
Il prend le chemin joueur retail uniquement si la victime appartient au registre
des participants et si la source, la victime, leurs incarnations et le cycle `4E`
correspondent aux identités publiées. Les véritables victimes NPC conservent leur
chemin natif. Le hook ne choisit pas de cible et ne réactive pas le cerveau Client.
Le shim préserve registres, flags et état flottant.

Cette dépendance incorrecte est démontrée dans le binaire. Son poids exact dans
le blocage observé, ainsi que le premier éventuel marker manquant entre Host et
Client, restent à établir avec une trace runtime des deux machines.

## Paired : conserver la véritable notification de fin

`FUN_00655A60` marque la fin victime (`4F+D8`) et appelle le finish source
`FUN_006559A0`. Ce dernier peut refuser de terminer immédiatement lorsque son
contrôleur d'animation n'est pas prêt. La seule inspection périodique du cycle
victime courant peut manquer la notification si le cycle `4F` a déjà été remplacé.

Le nouveau callback mémorise cette notification native pour la paire locale
exacte lorsqu'un Client observe une victime distante. Il valide le cycle avant
l'appel, puis les incarnations et l'identité de paire après l'appel, sans relire
l'ancien cycle potentiellement détruit. La preuve est effacée si la représentation
victime change. La reprise native du finish source introduite en v189 peut alors
utiliser cette preuve même après remplacement du `4F`.

Les HP ne constituent toujours pas une preuve de fin. Pendant qu'une présentation
fatale retient les snapshots suivants, les révisions de conséquence de **la même
paire** continuent désormais à progresser. Position, tâche et transitions de
cycle ultérieures restent différées. Deux compteurs facilitent la vérification :
`pairedRuntimePlayerRemaps` et `pairedLocalVictimFinishes`.

## Navigation et possession du mouvement

La chaîne native confirme une frontière utilisable sans Behaviour Tree Client :
mise à jour physique Character `007BFE50` → scheduler `00688640` → traitement des
requêtes `00682900` et mouvement `00688510`. Elle est distincte de la tranche de
décision IA supprimée par AISync dans `007342A0`.

La requête Move est créée par `00683BF0`, reçoit son objectif via `006777B0`,
est soumise par `00683CE0` et annulée par `00683DB0`. Sa mise à jour `006C7550`
est conditionnelle : l'objectif peut être en attente à `+34`, puis promu vers
`+1C`. AISync reconnaît maintenant les deux situations. Le Client poursuit un
objectif avec son propre contexte spatial plutôt que de dépendre exclusivement
de la direction calculée depuis la position Host.

La structure créée par `00464D00` à `Character+42A8` participe à cette chaîne,
mais les preuves disponibles ne permettent pas de la nommer « Path Corridor ».

Un échec ne condamne plus définitivement la même requête : réessai après une
seconde, avec conservation du repli locomotion et de la lease réseau existante.
Une réussite efface les jetons de rejet. Les requêtes refusées durablement ne
désactivent plus indéfiniment la récupération catastrophique existante ; les
seuils de distance et de durée restent requis. Aucune correction continue de
transform n'a été introduite.

Une transition acquittée cesse aussi de posséder le mouvement lorsque son cycle
local a changé ou porte les flags natifs de terminaison. Elle reste acquittée
pour éviter son rejeu. C'est particulièrement pertinent pour `3B`, dont le
constructeur demande déjà une terminaison. Les transitions non certifiées ne
reçoivent pas de bloc de paramètres copié aveuglément.

## Cibles, familles et coût réseau

Les candidats conservent la validation retail de durée de vie, d'hostilité et de
perception. L'admission d'un participant ne réactive plus un ancien record après
réutilisation de son adresse : owner et incarnation doivent correspondre avant
de toucher le record. Le helper natif qui recherche seulement l'adresse n'est
pas appelé tant qu'un record périmé la désigne.

Pour le contexte de compatibilité `THE_PLAYER`, AISync préfère maintenant le
participant effectivement sélectionné ou pending par retail, après validation,
avant le repli de proximité existant. Cela évite une divergence de contexte ;
cela ne démontre pas encore la durabilité de toute la chaîne de scoring et
d'arbitrage après la mort du Host.

`004124C0` confirme que `2` et `7` sont `ANDROID` et `ANDROID_HEAVY`, et `0`
est `PLAYER`. Native et Launcher admettent désormais `{1,2,3,4,6,7,9}`.
L'appartenance à PlayerSync reste vérifiée séparément. Cette correction ouvre
l'admission des Working Joes ; elle ne certifie pas toutes leurs attaques.

Les variations ordinaires de steering ne forcent plus un envoi IPC à chaque
frame : publication à 20 Hz, conservation des événements urgents. Les recherches
de Characters dormants évitent également les frames où aucun heartbeat ne peut
être publié.

## Validation et travail restant

`tools/research/aisync_native_audit.py` passe **34 vérifications statiques sans
échec** sur cet exécutable. La concordance des classes Native/Launcher et la
taille de l'état ont été vérifiées. `git diff --check` passe. La revue indépendante
des ABI, des verrous et de la possession du mouvement n'a pas trouvé d'anomalie
bloquante démontrée.

Les tests sources ont été complétés pour les familles NPC et la fin des leases
d'exécution ; ils n'ont pas été exécutés. Il reste à compiler puis vérifier :

1. Mort du Host observée par un Client : animation complète, fin source, reprise
   du mouvement, aucun maintien de la paire après sa fin native.
2. Host mort : perception, scoring, pending, promotion et poursuite d'un Client
   vivant par l'autorité Host, sans migration ni sélection forcée.
3. Courses et changements d'objectif : progression native Client, arrêt puis
   reprise, pas de grand rattrapage avant une attaque ; mesurer aussi le trafic.
4. Humains et Working Joes : navigation, interaction, attaques et conséquences
   vérifiées séparément pour chaque famille et chaque type d'action.

Le cycle `12 / EADB58` copie un bloc de `0x58` octets dont la sémantique portable
n'est pas certifiée. Les attaques à distance, certaines traversées, les
conséquences NPC-vers-NPC et une réplication générale des dégâts/morts NPC ne sont
pas intégralement certifiées par ce travail. La classe Facehugger `5` demeure
exclue et la capacité de suivi reste de 32 acteurs. Ces limites empêchent encore
de déclarer AISync terminé à 100 % ou de garantir toute la campagne coopérative.
