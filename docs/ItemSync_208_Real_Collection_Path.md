# ItemSync 208 — détection du vrai ramassage

- Launcher : `0.8.4.208`
- DLL : `0.11.4.208-itemsync-collect`
- Build de production : réussi
- Test en jeu : non fait

Analyse complète : `analysis/itemsync/collect_path_re.md`.

## Ce que montraient les diagnostics de la v207

| Point | Constat |
| --- | --- |
| Apparitions décidées par le Host | Elles fonctionnent. Les 64 propositions GCIP ont été décidées par le Host, puis appliquées sur les deux machines (`ownerSpawned: 64` sur chacune). Le Client avait tiré des objets différents (explosive, flare…) ; c'est bien le choix du Host qui a été appliqué. |
| Ramassage côté Host | Rien n'a été détecté : `collectSeen` et `otherCallers` valaient 0. Aucun `ItemPickupRequest` n'a donc été envoyé, et l'objet est resté chez le Client. |

## Cause

Le callback de sortie `0x007AFDC0` est enregistré sur chaque pickup créé, mais
aucun code du jeu ne l'appelle. `FUN_007A2B30`, que la v206 détournait, n'est
donc jamais atteint lors d'un ramassage.

Au ramassage, le jeu suit ce chemin :

```
utilisation -> UI_Icon "start" -> AddToInventory (FUN_00574EF0) donne l'objet
   -> "success" (FUN_008D1BB0) -> son -> le script de la composite se cache
```

La composite et son emplacement dans le gestionnaire de pickups restent en
place. Le propriétaire de l'objet ne reçoit jamais le message `0x20001F`.

## Correction

### Détection

Le module détourne `FUN_008D1BB0` (RVA `0x004D1BB0`), c'est-à-dire la sortie
« success » d'AddToInventory.

| Propriété | Valeur |
| --- | --- |
| Convention | thiscall |
| Arguments | `entityRef`, `ctx`, `double` |
| Retour | `RET 10` |
| Octets remplacés | 6 : `55 8B EC DD 45 10` |
| Appelant unique | `0x005751A1` (vérifié à l'installation) |

Ce détour n'est atteint que lorsque tout l'objet a été ajouté à l'inventaire.
Un ramassage partiel, un refus ou un retrait distant ne le déclenchent pas.

Pour retrouver l'emplacement du pickup, le module :

1. part de l'entité AddToInventory ;
2. remonte ses parents par `+8`, sur 4 niveaux au plus ;
3. retient l'emplacement dont la composite (`slot+0x70`) correspond à un parent.

En secours, il prend l'unique emplacement dont l'icône UI partage l'instance
de composite (`slot+0x5C`).

Sans emplacement trouvé, il s'agit d'un don d'inventaire scripté, pas d'un
objet au sol : le module l'ignore (`collectNoSlot`). Sinon, il lit la clé du
propriétaire et la position, puis le ramassage suit le chemin de la v206
jusqu'au Host.

### Retrait chez les autres joueurs

Le retrait passe toujours par `FUN_007A3200`. Le module efface d'abord le
propriétaire de l'emplacement. Le message « ramassé » n'est donc plus envoyé
chez les autres joueurs, comme chez celui qui ramasse : les scripts du niveau
réagissent de la même façon partout.

Le hook sur `FUN_007A2B30` est supprimé.

## Diagnostics

| Compteur | Sens |
| --- | --- |
| `collectSeen` | Succès d'AddToInventory |
| `collectNoSlot` | Don d'inventaire qui n'est pas un objet au sol |
| `collectFallback` | Emplacement trouvé par le secours `+0x5C` |
| `noOwner` | Objet lâché sans clé réseau |

Les autres compteurs sont inchangés.

## À vérifier en jeu

1. Le Host ramasse un objet : il disparaît chez le Client. Faire aussi
   l'inverse.
2. Sur la machine qui ramasse :
   - `collectSeen` augmente ;
   - `observedQueued` augmente ;
   - le log Launcher affiche « Local pickup … reported ».
3. Sur l'autre machine :
   - `applied` augmente ;
   - le log affiche « ITEM_SYNC_APPLIED » ;
   - il ne reste aucune invite « utiliser » à l'emplacement de l'objet.
4. Lâcher un objet, vérifier qu'il apparaît chez l'autre joueur, puis le
   ramasser : il doit disparaître chez le premier.
5. Si un objet reste visible, envoyer les deux zips de diagnostics. Les
   compteurs `collectNoSlot` et `readFaults` indiquent alors où la détection
   échoue.
