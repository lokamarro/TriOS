# Correctifs à appliquer aux sources d'origine

Appliqués automatiquement par `Scripts/integrate.sh` quand c'est
mécanique, listés ici pour audit. Rien d'autre n'est modifié dans
les moteurs : toute la fusion vit dans les adaptateurs TriOS.

## 1. Points d'entrée (obligatoire, mécanique)

| Fichier d'origine | Action | Pourquoi |
|---|---|---|
| `Henge/ios/Henge/HengeApp.swift` (`@main`) | Retirer `@main`, renommer en `HengeOriginalApp` inutilisé | Un seul `@main` par processus : celui de TriOS |
| `Kernarium/Kernarium/KernariumApp.swift` (`@main`) | Idem | Idem ; son `init()` (caches Metal/Mesa, variables d'env.) est recopié dans `EngineBootstrap.prepareProcess()` |
| `LinuxRunner/app/AppDelegate.m` + `main.m` iSH | Ne pas inclure dans le target TriOS ; rejouer ses initialisations utiles depuis `EngineBootstrap` | UIKit `UIApplicationMain` entre en conflit avec le cycle SwiftUI de TriOS |

## 2. Vues racines (mécanique)

- `Henge RootView/HomeView/SessionView`, `Kernarium ContentView`,
  `LinuxRunner ManagerView` : conservées comme référence dans
  `Vendor/`, **pas** compilées dans TriOS. Leurs écrans sont repris
  par `HengeModuleView`, `KernariumModuleView`, `LinuxRunnerModuleView`
  (même organisation, état branché sur `AppInstance`).

## 3. Chemins de fichiers (à faire moteur lié, non mécanique)

- Chaque moteur écrivait à la racine de son conteneur. Dans TriOS,
  rediriger leurs bases vers `Documents/TriOS/<App>/` :
  disques/aux Henge, rootfs LinuxRunner (`rootURL`), bundles et
  caches Kernarium. Les points exacts sont listés dans chaque
  `Docs/INTEGRATION_*.md`.

## 4. Symboles (à traiter si l'éditeur de liens se plaint)

- Préfixer ou cacher : symboles iSH partagés Kernarium/LinuxRunner,
  `libslirp` (Henge via Husk, Kernarium), symboles Mesa.
- Méthode : construire chaque moteur en bibliothèque à
  `-fvisibility=hidden`, ne ré-exporter que son API d'adaptateur
  (`henge_vm_*`, `kernarium_runtime_loader_*`, `LRRuntimeBridge`),
  et pour les collisions restantes, `objcopy --prefix-symbols`
  n'existant pas sur Mach-O, passer par un renommage de sources
  vendored (préfixe `hr_` / `kr_` / `lr_` sur la copie `Vendor/`).
