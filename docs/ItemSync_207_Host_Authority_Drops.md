# ItemSync 207 — objets au sol décidés par le Host et objets lâchés

| Élément | Valeur |
| --- | --- |
| Launcher | `0.8.4.207` |
| DLL | `0.11.4.207-itemsync-authority` |
| Branche | `feature/itemsync` |
| Profil de compatibilité du lobby | `:itemsync-v2` |

Rien n'a été compilé ni testé. L'analyse du binaire est dans
`analysis/itemsync/spawn_drop_re.md`. Elle a été réalisée avec Ghidra 12.1.2
(`F:\Alien Isolation MP\tools\ghidra`), sur un projet persistant de cet
`AI.exe` : `analysis_tmp/ai_00d6_ghidra`. Les adresses ont été vérifiées avec
capstone. Cette version complète la v206 ([ItemSync_206_World_Pickups.md](ItemSync_206_World_Pickups.md)).

## Ce qui change

1. **Mêmes objets au même endroit pour tous.** Seul `GCIP_WorldPickup` est
   aléatoire :
   - il tire son objet dans un pool par machine ;
   - dans un mode de difficulté, il saute deux apparitions sur trois.

   Ses apparitions passent désormais par le Host. `PickupSpawner` et
   `MultiplePickupSpawner` prennent leur objet, leur quantité et leur position
   dans les paramètres du niveau : ils restent natifs.
2. **Objets lâchés.** Un objet lâché par un joueur apparaît chez tous les
   autres, au même endroit et en même quantité. Le ramasser le retire chez tout
   le monde, avec le mécanisme de la v206.

## Apparitions `GCIP_WorldPickup`

Sur chaque machine, Host compris, en session multijoueur :

- Le module détourne la méthode d'apparition GCIP (`0x005CD430`). Ce détour
  sait quel propriétaire s'exécute.
- La demande native d'apparition (`0x007A2B90`) est bloquée quand elle vient du
  helper GCIP. Ce helper est reconnu par l'adresse de retour `0x005CD410` ; il
  sert aussi au chemin de restauration.
- Le résultat local est proposé au Host, une fois par propriétaire :
  - **apparition** : chemin de la composite, quantité, transformation 3×4 ;
  - **rien** : déjà ramassé, saut de difficulté ou pool vide. C'est le cas
    quand la méthode GCIP se termine sans demande d'apparition.
- Le Host garde la **première** proposition par propriétaire comme décision, y
  compris « rien ». Il la diffuse à tous les joueurs (message 127).
- Chaque machine applique la décision **quand son propre propriétaire
  s'exécute**. Si la décision arrive avant, elle est conservée jusqu'à ce
  moment. L'apparition native se fait par `0x007A2B90`, avec la clé du
  propriétaire. Le gestionnaire « créé » (0x1E) de GCIP retrouve alors l'objet
  comme d'habitude.
- Avant toute apparition, la DLL vérifie que le chemin reçu appartient à une
  définition d'objet de la base du jeu (`[racine+0x40]`).
- Si un retrait est déjà en attente pour ce propriétaire, l'objet n'apparaît
  pas : quelqu'un l'a déjà ramassé.

Le Host attend donc lui aussi sa propre décision, via son Launcher. Il n'y a
qu'un seul chemin, identique pour tous.

### Effets de bord côté Client

Le tirage local du Client (pool, générateur, compteur de difficulté) diverge de
celui du Host et n'est jamais utilisé. Les sauvegardes faites par un Client
contiennent donc ses propres valeurs. C'est sans effet tant que le Host décide.

## Objets lâchés

**Chez le joueur qui lâche l'objet.** Le module détourne `0x007A2D50` (lâcher
d'objet du joueur).

1. Le lâcher natif s'exécute normalement : inventaire, remboursement,
   indicateur `inventory+0x3BBC`.
2. Le module crée une clé de propriétaire synthétique :
   `{séquence, 0xA1D00000 | nonce}`. Aucune entité CATHODE ne porte cette clé.
3. Il envoie `{clé, chemin, quantité, position}` au Host.
4. Quelques pompes plus tard, l'objet apparaît. Le module écrit alors la clé
   dans le nouvel emplacement sans propriétaire du gestionnaire de pickups
   (+0x68/+0x6C) :
   - il choisit l'emplacement le plus proche de la position, à 1 m au plus ;
   - à défaut, il prend le seul nouvel emplacement ;
   - il abandonne au bout de 5 s (compteur `dropTagFailed`).

**Chez les autres joueurs.** Le module appelle `0x007A2B90` avec la même clé
synthétique et une matrice identité translatée à la position. Ce chemin ne
touche jamais à leur inventaire. Les messages « créé » et « ramassé » adressés à
ce faux propriétaire sont ignorés par le résolveur natif.

**Ramassage.** Le détour de collecte de la v206 lit la clé synthétique dans
l'emplacement. Le retrait se propage ensuite comme pour tout autre objet.

## Réseau

| Id | Sens | Rôle |
| --- | --- | --- |
| IPC 29 `ItemSyncSpawn` | DLL → Launcher | Proposition GCIP ou lâcher local |
| 126 `ItemSpawnProposal` | Client → Host | Proposition |
| 127 `ItemSpawnDecision` | Host → Clients | Décision |
| commande `ITEM_SYNC_SPAWN` | Launcher → DLL | Application d'une décision |

Contenu d'une proposition et d'une décision :

- `kind` : `owner` ou `drop` ;
- `ownerKey` : 16 caractères hexadécimaux ;
- `spawn` ;
- `path` : `Pickups\...`, ASCII imprimable, moins de 96 caractères ;
- `quantity` ;
- `transform` : 12 flottants.

Côté Host (`LobbyHost`) :

- `_spawnDecisions` garde la première décision par clé. Il est vidé au
  lancement et à la fin de la manche.
- Un Client qui devient prêt reçoit d'abord toutes les décisions, puis tous les
  objets ramassés.

Côté Launcher :

- Chaque Launcher applique les décisions `owner`, y compris celles qu'il a
  proposées. Il ignore ses propres `drop`.
- `ITEM_SYNC_SPAWN` et `ITEM_SYNC_APPLY` sont relancés toutes les secondes, au
  plus 30 fois, tant que la manche dure. Cela couvre les refus pendant le
  chargement (`EXPIRED`, `ITEM_SYNC_UNAVAILABLE`). Seuls `ITEM_SYNC_REJECTED`
  et `ITEM_SYNC_NATIVE_FAULT` sont définitifs.

## Vérifications à l'installation

`Install` vérifie les points suivants. Sans correspondance exacte, rien n'est
installé.

| Cible | Vérification |
| --- | --- |
| `0x007A2B90` | prologue `83 EC 68 8B 44 24 6C`, `RET 10` |
| `0x005CD40B` | `CALL` vers `0x007A2B90` |
| `0x005CD430` | `A1 ????????`, puis `83 EC 08 53 8B 98 A0 00 00 00` ; `mov al,1` à +0xA0 |
| `0x007A2D50` | `81 EC A8 00 00 00 56 57`, `RET C` |

Les détours renvoient vers le code natif tant que le module n'est pas activé, et
en dehors d'une session multijoueur.

## Diagnostics (`itemsync`, toutes les 5 s et dans le zip)

| Domaine | Compteurs |
| --- | --- |
| GCIP | `gcipRuns`, `ownerRequestsBlocked`, `ownerProposals`, `ownerNoneProposals`, `ownerDecisions`, `ownerSpawned`, `ownerSuppressedCollected` |
| Objets lâchés | `dropsObserved`, `dropsTagged`, `dropTagFailed`, `dropsSpawned`, `dropsDuplicate` |
| Anomalies | `unknownPaths`, `spawnFaults`, `ownerOverflow`, `spawnObservedOverflow` |

## Limites et risques

- **Apparition légèrement retardée.** Le Host aussi attend sa décision, ce qui
  fait un aller-retour local. Un Client attend un aller-retour réseau.
- **Moteur non vérifié en jeu sur trois points :**
  - qu'une clé synthétique est bien ignorée sans erreur par le résolveur ;
  - que la clé du propriétaire est identique sur toutes les machines ;
  - que la position de l'emplacement (+0x4C) correspond à l'objet.
- **Repli du marquage d'un objet lâché.** Si la position de l'emplacement est
  fausse, le marquage se rabat sur « le seul nouvel emplacement ». Cela
  fonctionne tant qu'un seul objet apparaît à ce moment-là.
- **`FUN_007AA500` et le joueur local.** Cette fonction prend le premier
  personnage de la liste comme « le joueur ». Il faut vérifier en multijoueur
  que c'est bien le joueur local.
- **Hors session.** En solo, ou avant la création du lobby, tout reste natif.

## À vérifier en jeu

1. Lancer une partie et comparer les objets au sol du Host et du Client au même
   endroit : même objet et même quantité. Un emplacement vide doit l'être
   partout.
2. Logs Launcher `ItemSync` :
   - « Local owner … proposed » sur chaque machine ;
   - côté Host, « ItemSync: owner … spawns … » ;
   - « Spawn decision … ITEM_SYNC_APPLIED » sur chaque machine.
3. Lâcher un objet chez le Host : il doit apparaître au même endroit chez le
   Client. Le ramasser chez le Client : il doit disparaître chez le Host. Faire
   aussi l'inverse.
4. Compteurs DLL :
   - `dropsTagged` doit suivre `dropsObserved` ;
   - `unknownPaths`, `spawnFaults` et `dropTagFailed` doivent rester à 0.
5. Envoyer les deux zips de diagnostics :
   - en cas d'objet absent partout (aucune décision appliquée) ;
   - en cas d'objet en double ;
   - si un objet lâché n'apparaît pas chez l'autre joueur.
