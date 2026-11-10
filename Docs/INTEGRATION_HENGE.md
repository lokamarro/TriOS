# Intégration Henge — rappel du câblage

Référence d'origine : `ios/Henge/SessionView.swift`, `Machines.swift`,
`Native/henge-vm.c`, `HengeBridge.h`.

1. Boot (déjà câblé dans `HengeAdapter`) : `henge_vm_share(...)` et
   `henge_vm_set_display(w,h)` avant `henge_vm_boot(rom, disk, aux, …)`.
2. Affichage : créer la `CAMetalLayer` dans l'onglet Session (reprendre
   le code de `SessionView`/`MacPresentation`), la passer à
   `henge_vm_set_display_layer(layer)` et la garder vivante pendant
   toute la session. Le snapshot `henge_vm_snapshot(...)` sert pour la
   miniature du switcher TriOS.
3. Entrées : reprendre `Trackpad.swift` et `Input.swift` d'origine
   (codes Linux pour `henge_vm_key`, HID pour le clavier matériel).
4. Installation : `Installer.swift` + `henge_vm_install(...)`, onglet
   Installer du module ; l'agent invité `henge-agent.gz` doit être
   copié en ressource (script de build d'origine, phase « Bundle the
   Mac helper » de leur `project.yml`).
5. JIT : leur dossier `ios/Henge/JIT/` (StikJIT intégré, appairage)
   se branche au lancement de TriOS, avant tout boot.

---
**Statut (mise à jour) : CÂBLÉ.** Boot : share + display avant `henge_vm_boot` ; affichage par `henge_vm_snapshot` + trackpad + touches spéciales dans `HengeDisplayView` ; installation via `henge_vm_install` exposée dans l'adaptateur (`install(mac:ipswPath:)` + `installProgress()`). Reste : le chemin CAMetalLayer plein débit de SessionView, qui peut remplacer le snapshot sans autre changement.
