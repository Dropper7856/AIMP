# AISync 188 — navigation native et propriété de l’exécution

Branche : `fix/aisync-native-execution`, basée sur `AISync-v2` / v187.
Launcher : `0.8.4.188`. DLL : `0.11.4.188-aisync-native-navigation`.

Cette version fournit des corrections compilées et des outils de validation.
Elle **ne constitue pas une validation à 100 % d’AISync en jeu**. Le ciblage
retail après la mort du Host et le blocage pendant une paire active restent
des critères d’acceptation ouverts.

## Ce qui change

La frontière préférée du mouvement devient la requête native générique
Move-to-point du scheduler `Character+2CD0`, avec la vtable RVA `00EB1A70`.
Le Host transporte son objectif résolu, sa distance d’arrêt et son type de
déplacement. Le Client crée une requête locale par `00683BF0`, initialise
l’objectif par `006777B0` et la soumet par `00683CE0`. Le moteur local exécute
la navigation, les collisions et la locomotion depuis sa propre position.
Le BT, la perception, les cibles et les décisions tactiques restent sur le Host.

Deux révisions distinguent le remplacement d’une requête de la modification
de son objectif. Une admission native Host crée une nouvelle époque, même
si l’allocateur réutilise la même adresse. Les changements d’objectif emploient
`006C7550` et sa file de cible native ; ils ne reconstruisent pas un chemin à
chaque heartbeat. Une requête terminée n’est pas relancée par un paquet identique.
Une requête échouée attend un nouvel objectif, au lieu d’être recréée chaque frame.

Les références natives sont détenues explicitement, libérées sur les barrières
de lifecycle/paire/destruction et expirent après 600 ms sans état Host frais.
Lors d’une invalidation de génération, leur libération ne consulte pas l’ancien
Character. Aucune position réseau n’est écrite dans l’adaptateur de navigation.
Les mouvements non reconnus conservent le carrier de steering existant comme repli.

La transition native `3B / 00EABEC0` est désormais certifiée. Son entrée
`006625E0` termine par `RET 4`, contrairement à ce que suggère la signature
décompilée. Le constructeur `00661270` ignore le mot transmis : l’appel Client
fournit zéro et respecte la pile. Capture et replay partagent la même table.

Une transition inconnue n’acquiert plus artificiellement le mouvement Client.
Une transition certifiée qui échoue reste réessayable ; son échec n’est plus
marqué comme un replay réussi. Navigation et steering reprennent lorsque
aucun cycle effectivement rejoué ne les possède.

Le watchdog v187 « victime morte + paire encore active + 350 ms → release »
est supprimé. La libération observateur fondée sur la vraie fin locale du graphe
victime reste présente. Les HP ne prouvent pas cette fin.
Le handoff neutre peut reprendre sur un nouvel objectif Host actif, même lorsque
sa direction finale de steering reste identique. La fermeture de l’ancien
objectif ne suffit pas à libérer ce handoff et l’ancienne paire reste mémorisée.
Une autorité dont l’exécution et les flags ont déjà quitté la paire peut aussi
reprendre immédiatement lorsqu’elle est arrivée avant la fin locale du graphe.

## Preuves statiques

Exécutable analysé : SHA-256
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.

`tools/research/aisync_native_audit.py` vérifie 27 frontières/octets/liaisons de
vtable dans `AI.exe`. Cela inclut les conventions d’appel des requêtes et des
callbacks paired. Le résultat local est 27 vérifications, zéro échec.

`Character+42A8` reste décrit comme l’objet de mouvement natif observé.
Cette modification ne le présente pas comme un Path Corridor démontré et
ne transporte aucune de ses listes, références ou structures internes.

## Format réseau

AISync passe de v10 / 256 octets à **v11 / 288 octets**.
Les champs existants, notamment outcome et execution, conservent leurs offsets.
Les 32 octets ajoutés contiennent uniquement des valeurs de navigation et
des révisions. Les validations Native et Launcher contrôlent les valeurs
finies, les bornes, les flags, les types de mouvement et les révisions.
Il faut utiliser la v188 sur toutes les machines ; v10 est rejeté.

## Vérifications effectuées

- DLL MSVC Win32 Release, C++20, `/W4 /WX` : compilation réussie.
- Test natif `AISyncOwnershipAndNavigation` : réussi.
- Test .NET `AISync authority codec` : réussi, incluant round-trip v11,
  conservation d’époque pendant une mise à jour et rejet des données invalides/v10.
- Non-régressions .NET `Native DLL priority` et `Generic native IPC` : réussies.
- Publication du Launcher x64 et de l’injecteur x86 : réussie.
- Audit binaire : 27 vérifications réussies.

Ces résultats ne remplacent pas une reproduction Host/Client dans le jeu.

## Diagnostics et acceptation en jeu

`aisync_navigation` expose capture, soumission, mises à jour, complétions,
échecs, annulations et expirations. `aisync_presentation` expose l’objectif,
les deux révisions et le propriétaire Client du mouvement :
0=aucun, 1=steering, 2=navigation, 3=execution, 4=paired, 5=neutre.

`aisync_paired_graph` trace les notifications natives avant/après exécution :
side 1=source (`00664A10`), 2=victime (`006658E0`), 3=finish source (`006559A0`).
Il expose l’événement, les identités, la paire et les flags natifs de progression.
Les workers ne lisent pas les Actors ; la file est bornée et non bloquante.
Ce flux est une observation, pas une autorisation de replay d’un marker.
Une absence d’événement ne constitue une preuve que si `pairedGraphDropped`
n’augmente pas pendant la fenêtre comparée.

Scénarios nécessaires :

1. Course vers un joueur, d’abord en ligne droite puis avec angle/obstacle :
   vérifier `captured/submitted/updated`, la locomotion visible et l’arrivée
   sans grand rattrapage spatial au début de la paire.
2. Client observateur du kill Host : aligner les traces par identités/paire,
   comparer l’ordre des événements et le premier changement source/victime
   présent sur Host et absent sur Client. Les ticks Windows des deux machines
   ne partagent pas une origine ; comparer les intervalles depuis l’admission.
3. Host mort, un puis plusieurs Clients vivants : vérifier une sélection retail
   durable et la reprise frontstage. Les traces `aisync_target_succession` et
   `aisync_participant_perception` existantes restent nécessaires.
4. Humain et Working Joe compatibles, perte temporaire du transport, retour
   backstage/frontstage, nouvelle birth et paire suivante : vérifier qu’aucune
   ancienne requête ou paire ne revient et qu’aucun BT Client ne fonctionne.

## Limites ouvertes

`12 / 00EADB58` transporte un bloc de paramètres de 0x58 octets dans le
constructeur retail ; sa reconstruction portable n’est pas démontrée.
Elle reste une métadonnée, avec reprise de mouvement contrôlée.
Toutes les variantes de déplacement et de traversal ne sont pas certifiées.

La disparition du premier événement paired pendant le kill n’est pas encore
établie par des traces Host/Client de cette instrumentation. Rejouer maintenant
`Start_Death` ou `End_Action` sur la seule observation des HP serait spéculatif.

La chaîne scoring/arbitrage/pending/promotion/selected du participant distant
n’est pas déclarée réparée. Les adaptations de contexte joueur post-mortem
déjà présentes dans v187 sont conservées ; elles ne prouvent pas à elles seules
qu’un Remote Character satisfait toute la chaîne de ciblage retail.

## Artefacts

Le dossier `dist/aisync-native-execution/Launcher` contient le backend publié,
l’injecteur x86 et `Native/AIMP.NetZ.dll`. Il ne remplace pas les installations
existantes. Le frontend Electron n’a pas été modifié. Les modules PlayerSync,
Equipment, WorldSync et PlayerLifecycle n’ont pas été modifiés.
