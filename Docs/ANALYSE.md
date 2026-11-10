# Analyse détaillée des trois codes sources

Analyse faite sur les archives fournies (Henge 0.2, Kernarium main,
LinuxRunner 1.1.1/905). Toutes les API citées ci-dessous ont été
relevées dans le code, pas dans les README.

## 1. Henge — macOS sur iPhone/iPad

| Aspect | Réalité du code |
|---|---|
| Langage / UI | SwiftUI + C11/ObjC (moteur), Metal pour le GPU paravirtualisé |
| Projet | XcodeGen : `ios/project.yml`, bundle `com.henge.app`, iOS 17+, arm64 |
| Point d'entrée | `@main HengeApp` → `RootView` : TabView (Macs / Install / Settings) + `SessionView` en plein écran par-dessus |
| API moteur | C globale, `ios/Henge/Native/henge-vm.h` : `henge_vm_boot/install/state/stop/snapshot/set_display_layer/share/key/mouse…` |
| État global | **Une seule VM par processus** (fonctions sans handle). L'app le dit elle-même : un seul Mac par lancement |
| Dépendances de build | `make ios-lib` + checkout **Husk** séparé (StikJIT, libslirp, glib, rppairing), `tools/build_ios_restore.sh` (libirecovery/usbmuxd/idevicerestore vendored) |
| Prérequis runtime | JIT obligatoire (StikDebug / TrollStore / jailbreak / StikJIT intégré iOS 26+), ~43 Go libres pour Sequoia, 4–8 Go de RAM guest |
| Licence | GPL-2.0-or-later |

## 2. Kernarium — vrai noyau Linux (User-Mode Linux)

| Aspect | Réalité du code |
|---|---|
| Langage / UI | SwiftUI (app) + C++20/CMake (runtime Darwin), noyau Linux 6.18 patché |
| Projet | `Kernarium/Kernarium.xcodeproj`, bundle `com.imxieyi.Kernarium` + projet sœur `Andarium` (Android) |
| Point d'entrée | `@main KernariumApp` → `ContentView` : MTKView guest + console série superposées par opacité (jamais démontées) |
| API moteur | C, `GuestHost/Sources/KernariumRuntimeLoader.h` : `kernarium_runtime_loader_select/run/request_stop/quiesce/resume/read_statistics…` |
| État global | **Runtime sélectionné une fois pour toute la vie du processus** (texte même de l'en-tête). Flavor Linux ou Android figé au premier démarrage |
| Dépendances de build | CMake ≥3.27 + Ninja, `dependencies/prepare.sh` (préfixes statiques privés vérifiés : KosmicKrisp/Mesa, zstd, gptfdisk…), XCFrameworks générés dans `.build/kernarium-xcframeworks`, bundles noyau « acceptés » séparés |
| Capacités | Noyau réel : namespaces, Docker, bureaux Linux, Wine/Proton, GPU Vulkan→Metal (KosmicKrisp), UBD/ext4, réseau libslirp. CPU arm64/arm32/x86_64, interpréteur + JIT |
| Licence | GPL-2.0-or-later (sauf en-têtes SPDX contraires) |

## 3. LinuxRunner — Linux userspace ARM64 (fork d'iSH)

| Aspect | Réalité du code |
|---|---|
| Langage / UI | Objective-C/UIKit (héritage iSH) + SwiftUI (`app/LinuxRunner/ManagerView.swift`, terminal xterm/Ghostty web) |
| Projet | `iSH.xcodeproj`, bundle `dev.linuxrunner.app`, cible iOS 15 (testé surtout sur iOS récents), build Meson/Ninja pour le cœur |
| Point d'entrée | `AppDelegate` UIKit (pas de `@main` Swift) |
| API moteur | Classe ObjC globale `app/LinuxRunner/RuntimeBridge.h` : `+[LRRuntimeBridge startRuntime/isRunning/runtimeStatus/stopDesktopSession/taskSnapshot…]` |
| État global | **Un seul runtime iSH par processus, un seul rootfs actif** (`activeRootName`/`nextRootName`), plusieurs environnements installés |
| Dépendances de build | Meson, Ninja, CMake, `ld.lld`, sous-modules (libapps, libarchive, glslang, spirv-cross) ; variante GPU : ANGLE + virglrenderer + libepoxy à révisions épinglées dans `docs/BUILDING.md` |
| Capacités | Userspace seulement (pas de boot noyau), 6 distributions, bureaux X11/Wayland, GPU virgl→ANGLE→Metal, JIT optionnel, interpréteur par défaut |
| Licence | GPL-3.0 |

## Ce que « une seule app » impose (contraintes iOS, pas des opinions)

1. **Un seul processus, un seul point d'entrée.** Trois `@main`/`AppDelegate`
   ne peuvent pas coexister. TriOS garde le sien et désactive les trois
   autres (PATCHES.md). Toute leur initialisation globale est rejouée
   dans `EngineBootstrap.prepareProcess()`, dans le bon ordre.
2. **Trois états globaux C dans le même espace d'adressage.** Aucun des
   trois moteurs n'a été écrit pour être instancié : Henge = fonctions
   `henge_vm_*` sans handle, Kernarium = loader à sélection unique,
   LinuxRunner = singleton iSH. TriOS les encapsule chacun dans un
   adaptateur, un par app, et n'essaie pas de créer deux sessions du
   même moteur.
3. **Collisions de symboles probables.** Kernarium et LinuxRunner
   descendent tous deux d'iSH (interpréteur, couche fs/kernel) ;
   Henge et Kernarium embarquent libslirp ; Mesa apparaît des deux
   côtés (KosmicKrisp chez Kernarium, virgl/ANGLE chez LinuxRunner).
   La parade, détaillée dans BUILD.md : compiler chaque moteur en
   framework/bibliothèque à visibilité cachée, préfixer les symboles
   exportés qui se recouvrent, ne jamais lier les trois `.a` « à plat ».
4. **Une seule limite mémoire (jetsam) pour les trois.** Henge seul
   demande 4–8 Go de guest + son JIT ; ajoute le noyau Kernarium et un
   bureau LinuxRunner et un iPhone « standard » ne suivra pas. Sur
   iPad M-series / iPhone haut de gamme avec l'entitlement
   increased-memory-limit, les trois sessions *légères* peuvent
   coexister ; sinon TriOS suspend Kernarium (vrai quiesce) et met en
   avant une app à la fois. L'interface, elle, garde toujours les
   trois sessions ouvertes et restaurables instantanément.
5. **Le JIT est un prérequis externe, pas un réglage.** Sans
   débogueur attaché (StikDebug), TrollStore ou jailbreak, iOS refuse
   la mémoire exécutable : Henge ne démarre pas, Kernarium et
   LinuxRunner retombent sur leurs interpréteurs. TriOS ne peut pas
   contourner ça, aucune app ne le peut.
6. **Un seul jeu d'entitlements et un seul bundle ID.** Celui de
   TriOS (`fr.trios.unified`, à changer pour ton équipe). Les
   entitlements « exotiques » des uns et des autres doivent être
   fusionnés et re-signés avec un certificat qui les autorise.

## Architecture TriOS retenue

- **OSKernel** : registre des apps, fenêtres, focus, z-order, RAM totale.
- **AppInstance** : une session par app, créée une fois, jamais détruite
  tant que le moteur vit ; la vue suit le même pattern que le
  `ContentView` de Kernarium (masquée, pas démontée).
- **EngineAdapter** (×3) : seule frontière vers le natif. Compile en
  mode « lié » (vrais appels, flags `*_LINKED`) ou « non lié »
  (l'interface tourne, l'adaptateur explique quoi construire).
- **Stockage isolé** : `Documents/TriOS/<App>/…` pour chaque moteur.
- **Fenêtres** : fermer ≠ quitter ; réduire ≠ suspendre ; le switcher
  (gestionnaire de tâches) montre les 3 sessions, leur état et leur RAM.
