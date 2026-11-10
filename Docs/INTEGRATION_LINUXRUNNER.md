# Intégration LinuxRunner — rejouer l'AppDelegate iSH

Référence d'origine : `app/AppDelegate.m`, `app/main.m`,
`app/LinuxRunner/RuntimeBridge.h/.m`, `docs/BUILDING.md` du repo.

1. Le runtime iSH attend l'initialisation que fait leur AppDelegate
   (filesystem `iOSFS`, fenêtre terminal, `prepareExecutionBackendAtLaunch`).
   Dans TriOS, ces appels sont rejoués depuis `EngineBootstrap` et
   le premier `LinuxRunnerAdapter.start()`, dans le même ordre.
2. Démarrage effectif (décommenter dans l'adaptateur, mode lié) :
   `LRRuntimeBridge.setNextRootName(activeRootName)` puis
   `LRRuntimeBridge.startRuntime()` ; vérifier `isRunning` /
   `runtimeStatus` et alimenter `instance.log` avec `diagnosticEntries`.
3. Le terminal d'origine (`TerminalViewController` + xterm/Ghostty web
   de `app/terminal/`) se monte dans l'onglet Terminal du module à la
   place de `LinuxTerminalView` (aperçu). Le bureau graphique passe
   par `LRRuntimeBridge.guiController()` / `desktopController()`,
   à présenter dans la fenêtre du module.
4. Données : `rootURL(name)` doit viser `Documents/TriOS/LinuxRunner/`
   pour ne pas partager la racine avec les deux autres moteurs.

---
**Statut (mise à jour) : CÂBLÉ.** `LinuxRunnerAdapter.start()` appelle réellement `setNextRootName` puis `startRuntime` ; l'onglet Bureau du module présente `LRRuntimeBridge.desktopController()` via `LinuxRunnerControllers.swift`. Reste : monter le TerminalViewController d'origine dans l'onglet Terminal à la place de l'aperçu.
