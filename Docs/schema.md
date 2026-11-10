# Schéma d'architecture (texte)

```
┌──────────────────────────── TriOS (1 processus iOS) ────────────────────────────┐
│  TriOSApp (@main unique)                                                         │
│    └─ DesktopRootView : bureau + dock + barre de statut                          │
│         ├─ AppWindowView(Henge) ─────── HengeModuleView ── HengeAdapter ──┐      │
│         ├─ AppWindowView(Kernarium) ─── KernariumModuleView ─ KernariumAdapter ─┤ │
│         └─ AppWindowView(LinuxRunner) ─ LinuxRunnerModuleView ─ LRAdapter ──┤    │
│  OSKernel : fenêtres, focus, z-order, RAM totale, switcher                     │ │
│  AppInstance ×3 : session vivante, jamais démontée tant que le moteur vit      │ │
└────────────────────────────────────────────────────────────────────────────────│─┘
        moteurs natifs (Vendor/, liés un par un, visibilité cachée)              │
   ┌──────────────────┬──────────────────────┬────────────────────────┐          │
   │ libhengevm.a     │ KernariumRuntime     │ runtime iSH/LinuxRunner│ ◄────────┘
   │ henge_vm_* (C)   │ loader_* (C, UML)    │ LRRuntimeBridge (ObjC) │
   └──────────────────┴──────────────────────┴────────────────────────┘
   Données : Documents/TriOS/Henge │ …/Kernarium │ …/LinuxRunner │ …/Shared
```
