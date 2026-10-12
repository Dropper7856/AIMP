# AISync 188 : déconnexion IPC du 12 octobre 2026

Les archives `AIMP-Diagnostics-20261012-104252.zip` (Host) et
`AIMP-Diagnostics-20261012-094256.zip` (Client) montrent un mélange de versions.

- Host : Launcher `0.8.4.187`, DLL `0.11.4.188-aisync-native-navigation`.
  Le contrôle HELLO refuse la DLL dès la connexion. Les deux lancements sont
  concernés, avant toute activation de la session AISync.
- Client : Launcher `0.8.4.187`, DLL
  `0.11.4.187-aisync-survivor-observer-continuity`. Les deux handshakes réussissent.
  Le diagnostic exporté indique `IpcConnected=true`.

Le log Launcher du Host indique :

```text
Native DLL 0.11.4.188-aisync-native-navigation is already loaded;
restart AI.exe to load 0.11.4.187-aisync-survivor-observer-continuity.
```

Le `PeekNamedPipe_failed` natif suit cette fermeture par le Launcher. Le dernier
processus Host répète 19 connexions/refus sans `ipc_hello_ack`. Le module AISync
s'installe, mais son rôle reste 0 et sa génération réseau reste 0. Ces traces
ne permettent donc pas de tester la navigation ou les interactions de la v188.

Le ZIP `AISync-188-native-navigation.zip` était un paquet backend/DLL/injecteur,
pas un Launcher Electron complet. Un raccourci vers l'ancien Electron continue
à démarrer son API 187, même si une DLL 188 existe dans le dépôt.

## Paquet complet

Le paquet Electron est construit dans `dist/aisync-188-desktop`, à partir de
l'interface Electron existante, avec :

- API self-contained win-x64 : Launcher `0.8.4.188` ;
- DLL x86 : `0.11.4.188-aisync-native-navigation` ;
- injecteur self-contained win-x86 ;
- configuration frontend existante conservée.

Le SHA-256 de la DLL reste
`877c99881e76fb9be1e0d9b1523987b9db60075f7c4beb69e1eed78ffb711367`.
Aucune logique IPC, WorldSync, PlayerSync ou AISync n'est changée pour ce
problème de déploiement. Le contrôle de version reste nécessaire : AISync 188
utilise le format d'état v11, incompatible avec l'ancien v10.

Vérifications effectuées : compilation du paquet Electron, architectures PE
des quatre exécutables/DLL, égalité du SHA-256 de la DLL embarquée et de la
DLL testée, versions attendue/embarquée identiques, API autonome démarrée puis
arrêtée normalement avec sa réponse HTTP `version=0.8.4.188`. Les tests
`AISync authority codec`, `Native DLL priority` et `Generic native IPC`
réussissent (3/3). Le test IPC utilise un client de test ; aucune nouvelle
partie Host/Client n'est lancée pendant ces vérifications.

## Installation et nouvel essai

1. Fermer le jeu et le Launcher sur les deux machines.
2. Utiliser `AI-MP-AISync-188-Portable-x64.exe` sur Host et Client, ou installer
   `AI-MP-AISync-188-Setup-x64.exe` et utiliser le nouveau raccourci.
3. Vérifier dans les nouveaux diagnostics `LauncherVersion=0.8.4.188`,
   `NativeDllVersion=0.11.4.188-aisync-native-navigation` et `IpcConnected=true`.
4. Refaire les scénarios AISync décrits dans
   `AISync_NativeNavigation_0.8.4.188.md`.

Le WorldManifest existant reste compatible. Une validation en partie Host/Client
est encore nécessaire après cette mise à jour.
