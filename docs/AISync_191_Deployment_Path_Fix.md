# AISync v191 — correction du chemin de déploiement

L'archive `AIMP-Diagnostics-20261013-094502.zip` démontre la cause du handshake
refusé : le Launcher annonce `0.8.4.191`, mais le bootstrap et le HELLO de la DLL
annoncent `0.11.4.188-aisync-native-navigation`. Le pipe se connecte à 09:44:44,
puis le Launcher refuse cette version et ferme la tentative. Les reconnexions
répètent la même incompatibilité. Le contrôle de version doit être conservé.

La DLL v191 avait bien été compilée par l'utilisateur, le 13 octobre à 09:40,
dans `AIMP.Native/out/build/vs2026-win32/bin/Release/AIMP.NetZ.dll`.
Le Launcher sélectionnait pourtant la DLL v188 du 12 octobre dans
`AIMP.Native/out/build/aisync-native-execution/bin/Release/AIMP.NetZ.dll`.
`NativeDllResolver` triait les dossiers de build par ordre alphabétique ; son
repli depuis la copie locale du Launcher ne considérait que le premier build.

## Réparation immédiate, sans build

La DLL **déjà compilée** `0.11.4.191-aisync-ipc-isolation` a été copiée vers :

- `AIMP.Native/out/build/aisync-native-execution/bin/Release/AIMP.NetZ.dll` ;
- `AIMP.Launcher/src/AIMP.Launcher/bin/x64/Release/net8.0-windows/AIMP.NetZ.dll`.

Les anciennes copies sont conservées dans
`analysis/deployment-backups/20261013-094835/`, avec un journal `deployment.json`.
Le format PE x86, la version embarquée et l'égalité SHA-256 des deux destinations
avec la source ont été vérifiés. Leur SHA-256 est :
`0a136a3404eaf36fa892b6620983271416e31cb1ad9a31fff5d97dca228e8380`.

Cette réparation bénéficie au Launcher v191 existant, sans recompilation.
Une instance du jeu qui aurait déjà chargé une ancienne DLL doit être fermée
entièrement puis relancée. Aucun processus utilisateur n'a été arrêté ou lancé.
La réparation concerne les copies locales de ce poste, pas celles d'un Client
sur une autre machine.

## Correction du code pour les prochains builds

Les candidats Native sont désormais triés par date de modification décroissante
du fichier DLL dans chaque configuration, avec un ordre de chemin déterministe
en cas d'égalité. Release reste prioritaire sur Debug ; les copies locales et
packagées conservent leur priorité lorsque leurs octets correspondent au build
retenu. Le handshake reste l'autorité finale sur la compatibilité de version :
une date récente ne prouve pas une version correcte.

Le test source `Native DLL priority` couvre désormais plusieurs builds, un
Release récent dans un dossier alphabétiquement postérieur, un Debug encore plus
récent, la préférence pour une copie locale identique et les dates égales.
Ce test n'a pas été exécuté. `git diff --check` passe ; aucune compilation ou
exécution du jeu n'a été effectuée. Les versions restent en **191** : cette
révision répare la sélection et le déploiement, sans changer le protocole ni la
DLL source. Aucune régénération du WorldManifest n'est requise.
