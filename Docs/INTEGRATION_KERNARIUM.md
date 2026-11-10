# Intégration Kernarium — câblage MTKView

Référence d'origine : `Kernarium/Kernarium/ContentView.swift` +
`EmulatorLauncher` (leur singleton) + `GuestHost/Sources/GuestDisplay.swift`.

1. Dans `KernariumModuleView`, remplacer l'aperçu « Écran guest » par
   la `GuestDisplayView` d'origine (UIViewRepresentable créant la MTKView).
2. Au premier affichage : `EmulatorLauncher.shared.start(view:geometry:)`
   fait en interne `kernarium_runtime_loader_select(flavor)` puis
   `kernarium_runtime_loader_run(view, boot)`. C'est cet appel que
   `KernariumAdapter.start()` doit déclencher via le launcher du
   module — d'où le point de branchement laissé dans l'adaptateur :
   il faut la vue réelle, pas un appel sans surface.
3. Reprendre tels quels : `SerialConsoleView`, `EmulatorConfiguration`,
   `GuestPersistence`, `BackgroundSession` (leurs dossiers de données
   pointent vers `Documents/TriOS/Kernarium/`).
4. Ne pas oublier l'ordre d'init : caches Metal/Mesa AVANT la première
   MTKView (déjà placé dans `EngineBootstrap`).

---
**Statut (mise à jour) : CÂBLÉ.** `KernariumAdapter.start()` implémente désormais la séquence complète (select figé, boot struct, run bloquant en arrière-plan, `onGuestExit`), `KernariumDisplayView` fournit la MTKView et l'enregistre dans l'adaptateur. Restent à rebrancher sur cette même vue : la saisie GuestInput et la console série d'origine.
