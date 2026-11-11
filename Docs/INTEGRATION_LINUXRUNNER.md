# Intégration LinuxRunner — rejouer l'AppDelegate iSH

Référence d'origine : `app/AppDelegate.m`, `app/main.m`,
`app/LinuxRunner/RuntimeBridge.h/.m`, `docs/BUILDING.md` du repo.

1. Le runtime iSH attend l'initialisation que fait leur AppDelegate
   (filesystem `iOSFS`, fenêtre terminal, `prepareExecutionBackendAtLaunch`).
   Dans TriOS, cette séquence doit être rejouée depuis le premier
   `LinuxRunnerAdapter.start()`, dans le même ordre — et NON depuis
   `EngineBootstrap.prepareProcess()` (lancement de l'app), qui ne
   contient qu'un commentaire l'interdisant explicitement.
2. Séquence de boot à rejouer (vérifiée le 2026-11-11 dans les sources
   d'origine, `Vendor/LinuxRunner/app/AppDelegate.m:528-533` et
   `app/LinuxRunner/RuntimeBridge.m:328-331,333,745`) :
   a. `LRRuntimeBridge.setNextRootName(activeRootName)` — choisit
      l'environnement à booter (déjà dans l'adaptateur).
   b. `LRRuntimeBridge.beginBoot()` — une seule fois par processus
      (`bootAttempted`, sinon NO) : variables `ISH_CODE_*`, fusion
      mémoire, backend Metal guest.
   c. `LRRuntimeBridge.prepareExecutionBackendAtLaunch()` — juste
      après `beginBoot`, avant tout bloc guest : observateur
      `DidBecomeActive` pour `retryJITWhenAuthorized`, gestion d'un
      crash JIT précédent, activation du JIT seulement si iOS
      l'autorise déjà (debugger attaché / région préparée).
   d. Le `boot()` de l'AppDelegate d'origine (`AppDelegate.m:116+`) :
      validation du rootfs (`root.tar.gz` du bundle + `Roots` :
      `meta.db` et `data` présents), `mount_root`,
      `become_first_process`, `prepare_guest_runtime_dirs`,
      `FsInitialize`, `prepareExchange`, nœuds `/dev`… Les fonctions
      C sont exposées (`mount_root` / `become_first_process` dans
      `kernel/init.h`) : c'est le morceau à rejouer côté TriOS.
   e. Si `boot()` renvoie 0 : `applySharedFolderOwnership`, puis
      `recordBoot` et le log `EXEC`.
   Point de blocage : le `startRuntime()` actuel
   (`RuntimeBridge.m:331`) renvoie vers
   `[(AppDelegate *)UIApplication.sharedApplication.delegate
   startLinuxRuntime]` — or l'AppDelegate d'origine est exclue du
   target TriOS (un seul `@main`). Il faut donc rejouer la séquence
   ci-dessus dans `LinuxRunnerAdapter.start()` au lieu d'appeler
   `startRuntime()`, ou exposer une nouvelle entrée native qui fait
   `beginBoot` + `prepareExecutionBackendAtLaunch` + `boot` sans
   passer par l'AppDelegate.
   Contrainte thread : `beginBoot` / `startRuntime` exigent le
   thread principal (`NSAssert`) — déjà garanti côté TriOS :
   `AppInstance.startEngine()` est `@MainActor`.
   Reste à vérifier sous Xcode : d'où vient le rootfs dans TriOS
   (bundle `root.tar.gz` ? `Documents/TriOS/LinuxRunner/` ? cf. §4),
   et que le `boot()` rejoué aboutisse.
3. Démarrage effectif (mode lié, une fois le rejeu ci-dessus en
   place) : séquence a–e, puis vérifier `isRunning` /
   `runtimeStatus` et alimenter `instance.log` avec `diagnosticEntries`.
   `LINUXRUNNER_LINKED` reste commenté dans `build_engines.sh` tant
   que ce rejeu n'est pas en place et vérifié sous Xcode.
3. Le terminal d'origine (`TerminalViewController` + xterm/Ghostty web
   de `app/terminal/`) se monte dans l'onglet Terminal du module à la
   place de `LinuxTerminalView` (aperçu). Le bureau graphique passe
   par `LRRuntimeBridge.guiController()` / `desktopController()`,
   à présenter dans la fenêtre du module.
4. Données : `rootURL(name)` doit viser `Documents/TriOS/LinuxRunner/`
   pour ne pas partager la racine avec les deux autres moteurs.

---
**Statut (2026-11-11, vérifié dans les sources) : NON CÂBLÉ.** `LinuxRunnerAdapter.start()` appelle `setNextRootName` puis `startRuntime`, mais `startRuntime` renvoie vers l'AppDelegate d'origine, exclue du target TriOS — le runtime ne peut pas démarrer ainsi. Reste : rejouer la séquence de boot (§2) côté TriOS, puis monter le TerminalViewController d'origine dans l'onglet Terminal à la place de l'aperçu. L'onglet Bureau du module présente `LRRuntimeBridge.desktopController()` via `LinuxRunnerControllers.swift`.
