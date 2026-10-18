# AISync 198 — route backstage du survivant après la mort du Host

Launcher : `0.8.4.198`. DLL : `0.11.4.198-aisync-backstage-route`.

## Symptôme

Diagnostics du 18 octobre (Host `AIMP-Diagnostics-20261018-115605`, Client
`AIMP-Diagnostics-20261018-105601`) : le mouvement de l'Alien côté Client est
correct. Après la mort du Host (17:54:23 UTC), l'Alien passe en conduit et ne
ressort plus pour attaquer le Client vivant.

## Preuve dans le diagnostic Host

- `aisync_target_succession` : l'ancienne cible est retirée (phase 5), puis
  plus aucune cible n'est sélectionnée.
- `backstageTransitions=1` : l'Alien n'est passé en backstage qu'une fois et
  n'en est jamais ressorti.
- Le réarmement backstage du survivant fonctionne : `backstageSurvivorRearms`,
  `backstageEligibilityOverrides` et `backstageNavigationPlans` valent 7812,
  sans échec.
- Mais dans les 93 échantillons `aisync_participant_perception` postérieurs à la
  mort, le cache du Client est présent et son objectif est actif, avec
  **`navigationRouteCount=1`** et **`backstageKeyA=0`** à chaque échantillon.

## Cause

`0040BBF0` n'appelle le parcours de route `0040AF70` que si le cache contient
plus d'un nœud (`+450 > 1`). `0040AF70` suit la route du joueur vers sa
destination et choisit les conduits de sortie, ou d'embuscade, entre 14 et 40 m
le long de cette route. En retail, la destination de `Character+3500` est
l'objectif du joueur (`set_objective_angle` → `006C5D70`).

Depuis la v187, le cache synthétique du participant recevait comme objectif
**la position du participant lui-même**. `00721270` calcule alors une route
d'un seul nœud. Aucun conduit n'est choisi et la machine d'état de sortie ne
démarre jamais. Le repli `0040AD00` exige `DAT_016AEAD4 == 1`, ce qui n'est pas
le cas ici.

## Correctif

`UpdateParticipantNavigation` choisit maintenant une vraie destination :

1. le dernier objectif actif lu sur le cache `+3500` du vrai joueur local
   (conservé après sa mort), s'il est à au moins 4 m du survivant ;
2. sinon la position de l'Alien qui exécute le balayage, s'il est à au moins
   4 m ;
3. sinon l'ancien comportement (position du survivant).

L'objectif appliqué n'est remplacé que s'il se déplace de plus de 3 m, pour
éviter qu'un nouveau calcul de chemin interrompe le précédent à chaque frame.
Aucune cible IA n'est injectée : la sélection du candidat, l'éligibilité et le
choix du conduit restent natifs.

Nouveaux compteurs dans `aisync_v2_snapshot` :
`backstageGoalsObjective`, `backstageGoalsOwner`, `backstageGoalsDegenerate`.

## Vérifications

- DLL Win32 Release (Ninja/MSVC) : compilation réussie, version
  `0.11.4.198-aisync-backstage-route` présente dans le binaire.
- CTest `AISyncOwnershipAndNavigation` et `AISyncHookInstallation` : 2/2.
- Launcher Release : compilation réussie, sans avertissement.
- Aucun changement de protocole réseau, de WorldSync ou du WorldManifest.

Non vérifié en jeu. Au prochain test, après la mort du Host :
`navigationRouteCount` doit dépasser 1, `backstageKeyA` doit devenir non nul,
`backstageTransitions` doit augmenter, puis l'Alien doit ressortir.
Si `backstageGoalsDegenerate` augmente seul, aucune destination exploitable
n'était disponible.
