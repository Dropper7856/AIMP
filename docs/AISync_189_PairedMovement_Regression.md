# AISync 189 — admission des paires et reprise du mouvement

Versions : Launcher `0.8.4.189`, DLL `0.11.4.189-aisync-paired-movement-handoff`.
Branche : `fix/aisync-native-execution`.

Le signalement concerne la disparition de l'animation de mort du Host chez
l'observateur après la v188, puis une source et une victime immobiles. Il n'y a
pas de diagnostic Client de cette partie. Les corrections reposent sur la
comparaison du code et les chemins natifs, sans prétendre établir la cause
unique du symptôme observé.

## Corrections

- Une interaction autoritaire résolue réserve le mouvement dès son admission,
  avant les attentes de binding, d'outcome préparé et de placement natif.
  La navigation locale est annulée et aucune nouvelle locomotion ou transition
  d'exécution n'est soumise pendant cette préparation.
- Les statuts de navigation 4/5/6 ne sont plus considérés comme un succès qui
  conserve le mouvement. Un échec reprend la consigne de locomotion Host déjà
  existante, sans libérer tout l'acteur. La même révision de requête/objectif
  rejetée n'est pas recréée chaque frame ; les nouvelles tentatives restent
  espacées d'au moins 250 ms après un échec.
- La libération ou suspension de l'exécution efface aussi son acquittement :
  une transition arrêtée peut ainsi être réacquise à partir de l'autorité Host.
- La création d'une requête de navigation Host ne court-circuite plus la cadence
  normale de publication du mouvement. Les chemins urgents paired/outcome sont
  conservés.
- Après preuve de fin du graphe victime local de la paire exacte, l'observateur
  peut retenter le finish natif de la source. `FUN_00655A60` appelle
  `FUN_006559A0`, qui peut refuser tant que le contrôleur d'animation source
  n'est pas prêt. La reprise vérifie le cycle 4E, son propriétaire, sa vtable
  et son indicateur de finish, avec un intervalle de 250 ms. Elle ne force pas
  la fin d'un graphe actif à partir de HP=0.

Compteurs ajoutés : `intentFallbacks`, `pairedObserverFinishRetries`,
`pairedObserverFinishCompleted`.

## Validation et limites

- Audit statique `tools/research/aisync_native_audit.py AI.exe` : 27 contrôles,
  aucun échec. SHA-256 du binaire :
  `00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
- Cas d'admission différée ajoutés à `AISyncPortableTests.cpp` ; non exécutés.
- Aucun build, aucune exécution du jeu. Les contrôles statiques ne prouvent ni
  le rendu de la paire en partie ni la fluidité des déplacements.
- Le gel historique pendant un graphe victime encore actif reste à vérifier :
  la reprise de finish vise uniquement le cas où ce graphe est déjà terminé.
- Le format réseau reste v11 / 288 octets. Aucun changement de WorldManifest ;
  aucune régénération n'est nécessaire pour ce correctif.

Pour la validation en partie, utiliser les deux composants 189 sur chaque
machine, observer le kill du Host depuis le Client, puis vérifier la reprise
de déplacement et les attaques contre le joueur survivant. Vérifier aussi une
course avec obstacles avant l'attaque et les transitions backstage/frontstage.
