# AISync 196 — exécution native et conséquences autoritaires

Branche : `fix/aisync-native-execution`.
Launcher : `0.8.4.196`. DLL : `0.11.4.196-aisync-native-execution`.

Ce lot implémente les carriers qui manquaient à la navigation, aux conséquences
NPC, aux tirs et aux paires contre des NPC. Les Clients conservent leur physique,
leurs graphes et leurs ressources natives. Le Host choisit les décisions et les
résultats gameplay. Aucun arbre de comportement Client n'est réactivé.

Les changements concernent AISync, son transport Launcher, ses tests et sa
version. WorldSync et PlayerSync ne sont pas modifiés. Le WorldManifest n'a pas
besoin d'être régénéré pour ce changement de protocole AISync.

## Implémentation

### Navigation

`AISyncNavigation` capture les demandes natives point, rotation, posture et
direction/distance (`1/4/7/9`). Les demandes de chemin `2` sont converties en
objectif point seulement après leur résolution native en coordonnées monde.
Les handles de waypoint et les corridors restent locaux.

Pour une demande directionnelle, les producteurs `00686570/00686860` permettent
de conserver le véritable objectif monde avant sa conversion en steering.
Quand cet objectif est disponible, le Client soumet une demande point locale.
Le moteur calcule ainsi un trajet depuis la position réelle du Client. La
direction/distance native reste utilisable lorsque cet objectif n'est pas connu.

Correction supplémentaire : après consommation d'un objectif point différé, la
destination acceptée est à `request+28`. `+1C` conserve un ancien cache. La lecture
du pending reste à `+34` lorsque son bit est présent.

Les rotations et postures courtes sont observées à l'admission, puis transportées
dans une file ordonnée. Leur révision de demande est conservée. Une commande
terminée identique ne redémarre pas sur une simple révision de métadonnées.
La capacité est de 256 NPC ; les bindings participants restent limités à 32.

Le paramètre d'une demande de navigation et l'allure physique sont distincts.
L'allure provient toujours de `Movement+194`. Les commandes locomotion existantes
restent le secours pour les demandes qui n'ont pas de carrier certifié.

### Santé, dégâts et joueurs

`AISyncHealth` observe le receiver commun `007CAAC0`, y compris les chemins qui
ne passent pas par le wrapper Character `007CC900`. Un événement conserve les
paramètres natifs, la santé avant le coup et le résultat observé sur le Host.
Le Client rejoue le wrapper natif ; son scope TLS substitue les PV résolus par
le Host dans `007B4AD0`. Les notifications natives continuent à s'exécuter.

Un snapshot absolu répare la santé sans rejouer un ancien coup à chaque heartbeat.
Les événements live sont marqués et ordonnés séparément des mouvements. Le curseur
de relecture distingue invocation et convergence : un postcontrôle différé ne
peut pas infliger une deuxième fois le même coup.

Les neuf arguments de dommage sont vérifiés sur les instructions. Les flags
passés sur un mot de pile sont normalisés à leur octet natif avant transport.
La réutilisation d'un emplacement comme variable flottante dans le décompilé
ne transforme pas le booléen d'entrée en paramètre flottant.

Les commits HP directs sont également observés lorsqu'aucun receiver de dommage
ou résultat paired ne les possède déjà. Leur événement porte `CommitOnly` et
rejoue le setter natif. Aucun coup ni montant de dommage n'est déduit d'une
différence de PV ; cela couvre notamment un kill direct sur un participant.

Les dégâts d'un NPC sur un participant ont leur propre carrier, rattaché au NPC
source et au peer victime. Ils peuvent donc atteindre le joueur local ou sa
représentation distante sans passer par une interaction couplée. Les receivers
Client refusent les conséquences locales de tirs NPC avant leur pré-réaction ;
les relectures autorisées passent par le wrapper natif avec le scope approprié.

Les dommages positifs retail provenant d'un Replica joueur sur le Host sont
refusés lorsque leur impact relève déjà du RPC Client. Les stimuli retail à
dommage nul conservent leur chemin Host. La sélection ne repose sur aucune
fenêtre temporelle ou comparaison approximative des positions.

Un NPC terminal conserve son exécution de mort. La réplication d'une attente ou
d'une locomotion ne la remplace pas. Les PV ne servent jamais de preuve de fin
d'un graphe couplé.

### Interactions couplées

Les victimes NPC utilisent une identité portable et une incarnation autoritaire,
en plus de leur lifetime local. Le chemin natif `4F` comporte explicitement
`ACT_MELEE_GRAB_MATCHED_PAIR_WITH_ALIEN_VS_NPC` ; il partage l'acquisition et les
notifications de graphe. La définition et les résultats Host sont consommés par
la préparation native avant ses branches joueur/NPC.

Les dégâts observés sous la soumission paired exacte ne deviennent pas un second
événement générique. Le dernier commit de santé conserve aussi sa provenance
source/pair. Le snapshot victime attend la consommation de ce résultat, ce qui
préserve l'admission même si le Host a déjà atteint ses PV terminaux.

Pour une représentation humaine distante, le chemin d'intégration couplée de
`006556D0` sélectionne la branche joueur après acquisition exacte `4E/4F`.
La branche NPC reste utilisée pour les victimes NPC. Les nouveaux hooks de source
et de markers observent les appels existants ; chaque original est appelé une
fois. Aucun marker, tick supplémentaire ou état de graphe n'est fabriqué.

### Tirs et animations scriptées

`AISyncCombat` capture les tirs acceptés après les décisions natives de cadence,
charge et cible. Les Clients soumettent la demande physique déjà choisie : état
arme, point de visée, munitions et notification Character. Leur décideur de tir
local est bloqué. Le pipeline natif de raycast/arme reste exécuté.

La sélection et le rangement utilisent la transition secondaire native
`00654E60`, à partir du slot et du descriptor Host résolus dans l'inventaire local.
Une ressource absente reste différée. La file de présentation est bornée et ne
bloque pas les résultats de santé suivants. Les demandes de visée sont annulées
aux barrières de cycle, aux expirations et aux changements de contexte.

Le cycle scripté `0x12/EADB58` dispose d'un carrier de valeurs explicites et d'une
relecture par `006579F0`. Le sous-ensemble admis possède des références spatiales
nulles, des ressources résolues localement et des flags connus. Les compteurs de
boucle mutables ne créent pas une nouvelle animation réseau. Le numéro de série
natif distingue deux exécutions successives du même type.

## Contrat réseau v12

Host et Clients doivent utiliser ensemble le Launcher et la DLL 196. Le préfixe
historique conserve ses offsets. La taille native est de **640 octets** :

| Offset | Carrier | Taille |
| --- | --- | --- |
| 192 | Résultat natif paired | 48 |
| 240 | Identité/révision d'exécution | 16 |
| 256 | Navigation typée | 60 |
| 316 | Santé du NPC | 16 |
| 332 | Cause portable de santé | 24 |
| 356 | Événement de dommage NPC | 64 |
| 420 | Animation scriptée | 52 |
| 472 | Conséquence sur participant | 72 |
| 544 | Incarnation/classe de victime NPC | 8 |
| 552 | État de combat | 36 |
| 588 | Tir accepté | 48 |
| 636 | Provenance paired de santé | 4 |

Les flags live `6/7/8/9` identifient respectivement dommage NPC, conséquence
participant, tir et demande courte. Leur histoire peut figurer dans un heartbeat
sans autoriser une nouvelle exécution. Le Launcher et la DLL conservent une file
ordonnée bornée à 1024 snapshots d'événements, en plus des mouvements coalescés.

## Preuves et validation

Exécutable certifié : SHA-256
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.

Les résultats REA/Ghidra sont conservés sous `analysis/rea-aisync-196` :

- `ev_def0dc7ed6824c512fef2424c0f1dce34bb8e3424e4756bcacca322afb4b45a4` :
  8 fonctions résolues sur 9 demandes, notamment santé, réaction et dispatcher
  de markers. Le callback `00691220` est certifié par les instructions PE.
- `ev_61da1fb87d0ca914077c97ac91572347dba247b810f72a78ecf4cecbebd1bdfc` :
  initialiseurs/updates scriptés. La sélection `006579E0` a résolu une autre
  fonction (`00657980`) et est exclue de la preuve de transition. L'entrée
  `006579F0` et son `RET 8` sont établis par le PE.

Les vérifications de navigation, d'équipement et de hooks combinent le décompilé
fourni, les résultats REA pertinents et les instructions du même PE. Le script
`tools/research/aisync_native_audit.py` reproduit **291 contrôles réussis**, dont
les littéraux utilisés réellement pour installer les hooks et leurs gardes ABI.

Compilations Release isolées : DLL MSVC x86 et Launcher .NET, sans erreur ni
avertissement du compilateur. Tests passés : contrats natifs AISync, codec AISync,
conservation de la connexion IPC après rejet AISync et frames du protocole.
Les sorties sont dans `analysis/aisync-validation-196` ; aucun déploiement ni
changement de l'installation du jeu n'est effectué.

Une vérification élargie rencontre deux fixtures WS3 existantes dans
`ProtocolTests.AssertWorldPublicAction` : leur golden utilise la version 6 tandis
que le codec accepte les racines 12/13. Ces deux fichiers sont inchangés par ce
lot. Les tests AISync et IPC passent indépendamment de ces fixtures.

## Limites de validation

Le jeu n'a pas été lancé. Les preuves statiques et les tests de contrat ne
démontrent pas le rendu complet, les collisions dans toutes les cartes ou la
continuité de chaque graphe. La disparition du gel visuel doit notamment être
observée en jeu avant de qualifier le module de terminé à 100 %.

Les graphes scriptés avec références externes, les demandes de navigation non
certifiées et les familles exclues comme Facehugger restent hors du sous-ensemble
rejoué. L'inventaire nécessaire doit exister localement. Une santé terminale locale
ne peut pas être ressuscitée dans le même Character ; une incarnation correcte est
requise. Le snapshot de PV terminaux établit une mort native et ne prouve pas à lui
seul une animation de mort particulière.
Une phase de santé intermédiaire sans transition locale correspondante reste
différée ; l'enum de santé n'est jamais forcé directement.
