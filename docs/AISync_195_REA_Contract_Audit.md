# AISync 195 — audit des contrats retail

Audit statique du 15 octobre 2026, sur `fix/aisync-native-execution`.
Sources préparées : Launcher `0.8.4.195`, DLL
`0.11.4.195-aisync-retail-contract-audit`. Aucun build AIMP ni lancement du jeu
n'a été effectué. Le banc de test du jeu est abandonné à la demande de l'utilisateur.

**Conclusion : l'autorité Host et plusieurs chemins d'exécution sont cohérents,
mais AISync ne réalise pas encore intégralement la synchronisation de toutes
les IA.** Quatre erreurs établies par le code et les instructions du jeu sont
corrigées. Les limites restantes sont détaillées ci-dessous ; une vérification
de signatures ne certifie pas une animation ou une partie multijoueur.

## Cible et périmètre

Le `AI.exe` du dépôt et celui de l'installation
`E:\SteamLibrary\steamapps\common\Alien Isolation` ont le même SHA-256 :
`00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364`.
C'est un PE x86, base préférée `00400000`. Les adresses de ce rapport sont des
VA préférées ; le code utilise des RVA et la base effective du processus.

L'audit confronte les résultats REA/Ghidra conservés, `AI.exe.c`, les instructions
PE et le code AISync Native/Launcher. Les lectures PE réalisées avec Python et
Capstone sont distinguées des preuves REA. Aucun autre module de gameplay n'est
modifié. `ProductInfo.cs` et la version DLL identifient les nouvelles sources.
Le WorldManifest conserve son format et ne doit pas être régénéré pour ce travail.

Un inventaire XML des ressources installées couvre les 86 fichiers directement
présents dans `DATA/BEHAVIOR`, sans erreur de parsing, et 191 classes de nœuds.
Il retrouve les actions partagées de déplacement, les déplacements Android avec
`Fastest:3` et `StoppingDistance:1`, ainsi que `ActionRangedShoot` chez les humains.
Cet inventaire ne prouve pas quel graphe ou BML est sélectionné à l'exécution.
Ses chemins, hashes et exemples sont conservés dans
`analysis/rea-aisync-195/behavior-inventory.json`.

## Corrections démontrées

### Requêtes de navigation terminées retenues par le scheduler

`00686D00` termine une requête avec le statut 3, puis appelle `00683470`.
Si `request+18` reste non nul, cette dernière conserve une référence dans la
liste terminale du scheduler à `+10`. `00682900` ne nettoie cette liste que si
`request+18 == 0`. AISync positionne cet octet à 1 à la création.

Le précédent `Release` sautait l'annulation lorsque `lease.finished` était vrai.
Il pouvait donc abandonner sa référence externe tout en laissant la référence
native et les parcours associés s'accumuler pendant la vie du Character.

`Release` appelle maintenant l'entrée retail `00683DB0` pour toute lease encore
vivante lorsqu'un retrait est demandé, même si elle est terminée. Cette fonction
désactive `+18` avant de retirer la requête via `006838D0` et `00683470`.
La référence AISync est ensuite libérée normalement. Le compteur `cancelled`
inclut donc aussi le retrait de requêtes terminales. Aucun effacement manuel des
listes natives n'est introduit.

### Protection paired incomplète pour les familles humanoïdes

Les callbacks scheduler `00687190` et `00687690` peuvent appeler `0065FC80` et
`0065FD60`. Pour les classes 2/3/4/6/7/9, ces entrées détruisent directement le
cycle courant avant de construire le suivant. Elles contournent le hook partagé
`0065BDD0` sur lequel reposait une partie de la protection v194.

Deux hooks protègent maintenant ces frontières avant le destructeur. Le premier
conserve ECX et son argument opaque avec `RET 4`, le second ECX sans argument
avec `RET`. Les instructions complètes, leurs opérandes SEH relocalisés et les
deux retours de chaque entrée sont vérifiés avant installation.

La protection reste limitée au Client, au contexte TLS des deux callbacks
scheduler et à une paire publiée, vivante, acquise et exactement liée. Les fins
natives hors de ce contexte et le teardown autoritaire restent libres.
L'Alien du dernier diagnostic est de classe 1 : cette omission ne démontre pas
à elle seule la cause de son gel. Elle démontre une lacune de couverture pour
les autres familles visées par AISync.

### Une victime interrompue pouvait retenir la source

Dans `006658E0`, l'événement `Start_Death` d'une victime interactive (`+DA != 0`)
peut poser `victimCycle+8 |= 6`, puis retourner sans passer par `00655A60`.
Les octets de fin `D8` victime et `E6` source peuvent donc rester nuls.

Le guard v194 ne lisait pas ces flags victime. Il pouvait continuer de retenir
la source malgré cette interruption native. Il laisse maintenant passer le
bit 4 victime, comme il le faisait déjà pour l'échec source. Le bit 2 isolé
n'est pas assimilé à une fin de graphe. Aucun test `HP == 0` n'est ajouté.

### Les impacts Client transportaient une incarnation locale

`00783B00` expose `Character+50`. Les sense records conservent ce jeton dans
`record+430` pour vérifier une durée de vie locale ; il ne constitue pas une
identité portable entre processus.

Le Client envoyait pourtant ce runtimeId comme `targetBirth`, et le Host le
comparait à son propre runtimeId. Un même NPC peut porter 91 sur le Client et 3
sur le Host. Un impact valide était alors rejeté. À l'inverse, une collision de
jetons ne prouvait pas l'incarnation autoritaire du destinataire.

Le Client envoie maintenant la `remoteBirth` AISync annoncée par le Host. Le Host
la compare à `actor.birth`. Chaque côté vérifie séparément son propre runtimeId,
l'acteur admis, l'identité, la session et le monde. Un impact ne peut pas créer
une admission d'acteur. Les accès à `gActors` restent sur le thread moteur.

Le message d'impact passe à la version 2, toujours sur 64 octets. La version 1
est rejetée explicitement pour éviter une confusion silencieuse des identités.
L'état AISync reste en v11, sur 288 octets natifs. Les fixtures codec et IPC sont
adaptées ; le test IPC inclut le rejet d'un impact v1 suivi d'un impact valide.
Les sources des tests natifs couvrent les runtimeId différents, les anciennes
incarnations et la réutilisation locale d'adresse. Ces tests compilés n'ont pas
été exécutés.

## Contrats confirmés et couverture restante

| Question | État de preuve | Résultat |
| --- | --- | --- |
| Un seul cerveau Host | Établi statiquement sur les frontières examinées | Pour les familles admises, le Client saute la tranche manager `0073433E..007344D3`, le coordinator et le pré-arbitrage ; les mises à jour physiques restent distinctes. Aucun transfert de rôle sur mort du joueur Host. |
| Exécution native d'un objectif Move | Établie pour le type admis | Factory `00683BF0`, objectif `006777B0`, submit `00683CE0`, update différé `006C7550`, cancel `00683DB0`. Les références et ABI examinées concordent. |
| Locomotion complète | Partiellement couverte | Capture limitée à kind 1 / vtable RVA EB1A70. Kind 9 / EB19AC et cycle 12 / EADB58 restent incomplets. La convergence v194 est un repli borné, pas une preuve de capture de tous les objectifs. |
| Paired humaines | Partiellement établie | Les traces v193 prouvent le démarrage natif 4E/4F puis le remplacement prématuré de la source. L'attribution exacte à un producteur scheduler et le premier callback éventuellement absent restent à mesurer. |
| Fin paired | Chemins natifs identifiés | `Start_Grapple` → stage 1 → `006638D0` → stage 2 → événement victime → `00655A60` / D8 → `006559A0` / E6 si contrôleur prêt → stage 6. HP seul n'est pas une preuve. |
| Cinématique paired complète | Non certifiée | `006556D0` distingue encore un joueur retail d'un proxy NPC pour l'intégration couplée des mouvements. Cette divergence mérite une analyse ciblée ; sa responsabilité dans le gel n'est pas démontrée. |
| Participants dans le sélecteur retail | Partiellement établie | Les adaptations de candidat/record conservent hostilité et lifetimes ; aucun forçage selected/pending trouvé. Scoring, promotion et sélection durables après mort Host ne sont pas intégralement prouvés. |
| Compatibilité THE_PLAYER après mort | Heuristique existante | Le contexte préfère selected/pending retail puis un participant vivant proche. Il ne modifie pas directement le sélecteur, mais peut influencer les consommateurs solo du slot joueur. L'admission totalement retail reste un objectif ouvert. |
| Dégâts et morts NPC hors paired | Couverture manquante | Le Client supprime les impacts locaux, le Host applique le dégât, mais State v11 n'annonce pas un état général de santé/mort NPC. Aucun chemin général de conséquence de décès n'est certifié par cet audit. |
| Réactions aux impacts sans dégâts | Couverture manquante | `007CDCF0` et `007CE020` comportent des réactions et callbacks distincts des dégâts. AISync refuse le dégât nul et rejoue seulement `007CC900`. La correction d'identité ne couvre pas ces réactions. |
| Humains, Joes et campagne complète | Non certifiés | L'admission de classe est générique, mais tirs humains, conséquences NPC-vers-NPC, traversées et paires avec victimes NPC ordinaires ne sont pas intégralement couverts. Facehugger classe 5 exclue ; capacité de 32 acteurs. |

Le dernier essai utilisateur montre déjà une attaque du Client après la mort du
Host. C'est une observation utile de succession ; elle ne garantit pas toutes
les durées de poursuite, tous les niveaux ou toutes les familles NPC.

La chaîne physique distincte est `007C2450` → `007BFE50` → `00688640` →
`00682900` / `00688510`. Cela ne suffit pas à certifier l'indépendance de tous
les callbacks d'animation. Les composants virtuels de la tranche manager
supprimée ne sont pas tous classifiés par cet audit. En particulier, le simple
préambule `007ACCF0` ne constitue pas une preuve de mise à jour des graphes
4E/4F. Cette frontière doit rester une question explicite de l'analyse des gels.

La distribution native des markers est reliée plus précisément : l'initializer
4E `0066BDE0` enregistre notamment `Start_Grapple` et `End_Action` via `006980E0`,
puis peut appeler directement son propre `virtual+20`. Le dispatcher
`00691000` distribue les événements de `graph+A8[index]` aux listeners
`graph+84[index]` par `virtual+04`. Ce slot mène à `00664A10` pour 4E et à
`006658E0` pour 4F. Le callback `00691220`, référencé dans la table `012B23E4`,
appelle ce dispatcher ; sa construction est identifiée à `00697D65` et son
stockage dans `graph+80[index]`. Ces observations viennent du décompilé fourni
et du PE. Elles ne démontrent pas encore l'invocation continue de ce callback
sur chaque famille de Character.

Une recherche bornée dans le décompilé et les chargements PE de `+36C` n'a pas
relié le tick continu du cycle à sa racine moteur. Cette absence de résultat
n'est pas une preuve d'absence : une liste, une référence intermédiaire ou un
job peut porter cet appel. Aucun appel de graphe n'est ajouté sur cette seule
base, afin de ne pas risquer de le mettre à jour deux fois.

## Preuves et vérification

Les lots REA historiques sont conservés dans `analysis/rea-aisync/index.json`
et `evidence.json`, avec leurs pseudocodes. Identifiants particulièrement utiles :

- Scheduler : `ev_1c21cd912c06c5adde1e8923d72ce2367feb10ff33bb534daf842bfdfdd473f3`.
- Exécution/navigation : `ev_2c8f7c736c0a05590b9fb76c4b00f4c8a8f3db50e352be227d779ab059daee22`.
- Carriers paired : `ev_c2d87787ffe26d7ebe9ddcfdcad2449019831428406800660e56905e5b15e68d`.
- Autorité : `ev_d7ade1fd52f876e8436a04cb36e063804701611487967298100438d71d226b03`.
- Candidats/records : `ev_aab0bc659f5d3bcfc32d196c8fafc6a8f56eb3310441e5a1d7d5a3c186bb7e4e`.

La nouvelle passe REA est conservée séparément dans `analysis/rea-aisync-195`.
Une première tentative MCP a dépassé 300 secondes pendant l'import Ghidra et
n'a produit aucune nouvelle décompilation exploitable. La reprise utilise le
REA installé par son transport stdio local, avec un délai adapté. Elle est
terminée et sa session native est fermée. Le lot
`ev_62d6b1a572b2dcdf58e3927de4d07ca22cc2d28730ec763e4cbfb4b36cb3cf50`
contient huit procédures résolues/décompilées sur douze sélecteurs :
`0065FC80`, `0065FD60`, `00683DB0`, `006838D0`, `00783B00`, `007CDCF0`,
`007CE020`, `006556D0`. Les quatre autres entrées (`00681270`, `00679A30`,
`006815A0`, `006813A0`) ne sont pas reconnues automatiquement comme fonctions
par Ghidra. Cela ne signifie pas qu'elles ne contiennent pas du code. Les
observations PE supplémentaires ne sont pas rebaptisées décompilations REA.
Le fichier `contracts.json`, l'index, les pseudocodes et l'export complet
`evidence.json` conservent ces résultats et leurs limites.

Cette nouvelle preuve confirme le contournement du hook Idle partagé, le
retrait natif des requêtes terminales, l'identité locale `+50` et les chemins
d'impacts qui dépassent le seul dégât HP. La signature décompilée de `0065FC80`
ne représente pas correctement son argument pile ; ses instructions `RET 4`
restent la preuve d'ABI utilisée par le correctif.

`tools/research/aisync_native_audit.py` vérifie le hash cible, les frontières
indépendantes et les signatures réellement utilisées à l'installation. Les
nouvelles instructions de destructeur, de rétention du scheduler et d'échec
victime font partie des contrôles. Les revues indépendantes des changements
paired et impacts n'ont trouvé aucune erreur bloquante supplémentaire.

Les **204 contrôles statiques passent**, dont 111 signatures runtime AISync et
39 préfixes de hooks, ainsi que les deux relocations HIGHLOW des nouveaux hooks.
L'alignement des versions Native/Launcher/protocole et `git diff --check` passent.
Ce sont des vérifications de fichiers et d'instructions, pas des tests compilés.

L'acceptation runtime reste ouverte. Aucun de ces contrôles ne justifie de
déclarer AISync terminé à 100 %. La priorité fonctionnelle suivante est de
compléter les carriers natifs manquants avant d'élargir la promesse de prise en
charge à toutes les IA.
