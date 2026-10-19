# ItemSync 206 — synchronisation des objets ramassables

- Launcher : `0.8.4.206`
- DLL : `0.11.4.206-itemsync`
- Branche : `feature/itemsync`
- Profil de compatibilité du lobby : suffixe `:itemsync-v1`

Rien n'a été compilé ni testé. L'analyse du binaire est dans
`analysis/itemsync/pickup_re.md`. Les adresses et conventions d'appel utilisées
ici ont été revérifiées au désassembleur.

REA n'a pas pu servir : son fournisseur Ghidra refuse
`C:\Users\donov\AppData\Local`, qui est une jonction (« Reparse path component
is unsupported »). L'analyse a donc été faite sur `AI.exe.c` et avec capstone.

## Comportement

1. Un joueur ramasse un objet au sol.
2. Le Host retient le premier joueur qui l'a ramassé pendant la manche.
3. Les autres joueurs voient l'objet disparaître. Ils ne le reçoivent pas.
4. Un Client qui termine son chargement après des ramassages reçoit la liste
   complète au moment de son `GameplayReady`.
5. Si l'objet n'existe pas encore chez un joueur (zone non chargée, apparition
   tardive), son retrait reste en attente. Il est appliqué dès que l'objet
   apparaît.

## Fonctionnement natif des objets ramassables

Un objet au sol n'est pas une entité du niveau. Une entité propriétaire placée
dans le niveau (`GCIP_WorldPickup`, `PickupSpawner` ou `MultiplePickupSpawner`)
demande au gestionnaire de pickups (`[0x012F0DE8]`, noté PM) de créer une
composite `Pickups\<nom>`. Le PM range chaque objet créé dans une table de
0x400 entrées de 0x9C octets, qui commence à PM+0x3C.

| Champ | Contenu |
| --- | --- |
| +0x4C | position (float3) |
| +0x68 / +0x6C | clé de l'entité propriétaire |
| +0x70 | référence vers la composite créée |
| +0x96, bit 0x04 | entrée occupée |

Chaîne de ramassage :

```
0x007AFDC0  callback de sortie de la composite (vérifie "pickup_collected")
  -> 0x007AFDE4  CALL 0x007A2B30      demande de retrait (thiscall PM, ref*, RET 4)
       -> différé par le bus de messages local
  -> 0x007A3200  retrait (thiscall PM+0x1C, clé composite*, RET 4)
       message 0x20001F au propriétaire, destruction de la composite (+0x2D |= 1),
       libération de l'entrée
```

L'objet est donné au joueur avant cette chaîne, par `AddToInventory`.
`0x007A3200` ne touche jamais à l'inventaire.

## Détection (DLL)

- Le module détourne `0x007A2B30` (6 octets `8B 44 24 04 8B 00`, sans
  opérande relatif).
- Seul l'appel dont l'adresse de retour vaut `0x007AFDE9` est un vrai
  ramassage. L'autre appelant connu, le despawn de `PickupSpawner`, est ignoré.
- Le détour lit la clé de la composite (`0x008C3360`), puis son entrée dans la
  table (`0x00793F30`). Il en tire la clé du propriétaire et la position.
- Un objet sans propriétaire (lâché par un joueur) n'a pas d'identité stable :
  il n'est pas synchronisé (compteur `noOwner`).
- `Install` vérifie les octets des cinq fonctions, la cible du `CALL` à
  `0x007AFDE4` et les `RET 4`. Sans correspondance exacte, rien n'est installé.

## Identité d'un objet

La clé tient dans 32 caractères hexadécimaux (`ItemSyncKey.h`) :

- `low` : clé CATHODE du propriétaire, `{ShortGuid, id d'instance de
  composite}`. C'est la même forme de clé que WorldSync. Elle est identique sur
  chaque machine qui charge le même niveau.
- `high` : marqueur `0x4954` (« IT ») et position quantifiée à 0,1 m (int16 par
  axe).

La position ne sert qu'à `MultiplePickupSpawner`, dont tous les objets
partagent le même propriétaire. La clé de la composite créée n'est jamais
utilisée : elle dépend de l'ordre d'apparition des objets sur chaque machine.

## Retrait chez les autres joueurs

Le retrait s'exécute sur le thread moteur, via la commande `ITEM_SYNC_APPLY`.

1. Le module parcourt les entrées occupées dont le propriétaire correspond.
2. Une seule entrée trouvée : elle est retirée. Si sa position diffère de plus
   de 3 m, le compteur `positionMismatch` augmente.
3. Plusieurs entrées : la plus proche à moins de 1,5 m est retirée. Sinon le
   retrait reste en attente (compteur `ambiguous`).
4. Le module appelle `0x007A3200(PM+0x1C, clé de la composite)`. Le propriétaire
   reçoit son message « ramassé ». Pour `GCIP_WorldPickup`, cela enregistre la
   persistance de l'objet, qui ne réapparaît donc pas.
5. Le module retient chaque objet déjà retiré ou ramassé localement, par
   génération de niveau. Une seconde demande renvoie `ITEM_SYNC_ALREADY_APPLIED`.
6. Les retraits en attente sont réessayés toutes les 250 ms depuis la pompe
   `CharacterManager`. Ils sont abandonnés au changement de niveau.

Ce retrait n'est pas renvoyé au Host : il ne passe pas par `0x007A2B30`.

### WorldSync

Les messages typés du PM sont désormais traités en local seulement par WS3
(`ReceiverLocalInteractionControlMessage`) : `0x20001E` (objet créé),
`0x20001F` (objet ramassé), `0x200034` (utilisation), `0x200036` et `0x200037`
(entrée et sortie de portée). Sinon un autre joueur recevrait « ramassé » sur le
propriétaire sans que l'objet soit retiré.

## Réseau

| Id | Sens | Contenu |
| --- | --- | --- |
| IPC 28 `ItemSyncPickup` | DLL → Launcher | `{schemaVersion:1, sequence, entityId}` |
| 124 `ItemPickupRequest` | Client → Host | `SessionId, PeerId, LevelGeneration, EntityId` |
| 125 `ItemCollected` | Host → Clients | `SessionId, LevelGeneration, EntityId, CollectorPeerId` |
| commande `ITEM_SYNC_APPLY` | Launcher → DLL | `entityId` |

Côté Host (`LobbyHost`) :

- `_collectedItems` associe chaque clé au premier joueur qui l'a ramassée. Il
  est vidé au lancement et à la fin de la manche.
- Une seconde demande pour la même clé est ignorée et journalisée.
- La diffusion est envoyée à tous les Clients. Chaque Launcher ignore les
  objets qu'il a lui-même ramassés.
- Le rejeu est envoyé à un Client à la réception de son `GameplayReady`.

Le module de coordination est `MultiplayerSessionCoordinator.ItemSync.cs`.

## Limites connues

- Si deux joueurs ramassent le même objet presque en même temps, chacun le
  garde : il n'y a pas d'annulation locale. Le Host journalise le doublon.
- `GCIP_WorldPickup` tire son objet au hasard, et dans un mode de difficulté
  seul un objet sur trois apparaît. Deux machines peuvent donc avoir un objet
  différent, ou aucun, au même endroit. Un retrait sans objet local reste en
  attente sans effet.
- La position de l'entrée (+0x4C) est déduite de l'analyse, pas vérifiée en
  jeu. Si elle est fausse, seuls les `MultiplePickupSpawner` à plusieurs objets
  sont touchés (`ambiguous`).
- Le retrait déclenche la réaction du propriétaire : `pickup_collected` pour
  GCIP, `collect` pour `PickupSpawner`. Les scripts qui en dépendent
  s'exécutent donc aussi chez les autres joueurs.

## À vérifier en jeu

1. Le Host ramasse un objet : il disparaît chez le Client, et le Client ne le
   reçoit pas. Faire l'inverse.
2. Le log Launcher `ItemSync` affiche « Local pickup … reported », puis chez
   l'autre joueur « removed locally: ITEM_SYNC_APPLIED ».
3. Le log DLL `itemsync` (toutes les 5 s et dans le zip de diagnostics) :
   `collectSeen` augmente à chaque ramassage, et `applied` chez l'autre joueur.
   `readFaults`, `applyFaults`, `positionMismatch` et `ambiguous` doivent
   rester à 0.
4. Un objet ramassé ne réapparaît pas après un rechargement de zone.
5. Envoyer les deux zips de diagnostics si :
   - un objet reste visible ;
   - `deferred` augmente sans `pendingApplied` ;
   - un ramassage ne donne pas l'objet localement.
