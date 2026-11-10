# TriOS — Henge + Kernarium + LinuxRunner dans une seule application

Une seule app iOS/iPadOS, façon OS : un bureau avec des icônes, un dock,
un gestionnaire de tâches. Tu lances Henge, Kernarium et LinuxRunner
depuis le bureau, tu passes de l'un à l'autre, tu réduis, tu rouvres —
**les trois sessions restent vivantes en même temps**, aucune ne
redémarre quand tu changes de fenêtre.

Schéma d’architecture : `Docs/schema.md`

## Compiler (résumé)

```bash
# Étape 1 — l'interface complète, sans les moteurs natifs (2 minutes)
open TriOS.xcodeproj        # Xcode 16+, 26 recommandé
# choisis ton équipe de signature, cible iPhone/iPad arm64, Run.
```

L'app démarre, le bureau fonctionne, les trois modules s'ouvrent,
mais leurs moteurs affichent « Moteur non lié ». C'est normal et
voulu : c'est l'étape 2 qui les branche.

```bash
# Étape 2 — brancher les vrais moteurs (plusieurs heures la 1re fois)
./Scripts/integrate.sh ~/Downloads/Henge-0.2.zip ~/Downloads/Kernarium-main.zip ~/Downloads/LinuxRunner-main.zip
./Scripts/build_engines.sh          # lit Docs/BUILD.md avant !
open TriOS.xcodeproj                # les flags *_LINKED sont posés par le script
```

**Lis `Docs/ANALYSE.md` avant de lancer l'étape 2.** Il explique,
code d'origine à l'appui, ce qui peut vraiment tourner en simultané
sur iOS et ce qui atteindra les limites mémoire de l'appareil :
les trois moteurs dans un même processus partagent UNE limite
jetsam. TriOS prévient, suspend et priorise, mais ne peut pas
faire ce qu'un vrai OS ferait (tuer un seul processus fautif).

## Structure

```
TriOS.xcodeproj          projet prêt à ouvrir (project.yml = source XcodeGen équivalente)
TriOS/
  TriOSApp.swift         LE seul @main (voir Docs/PATCHES.md)
  Core/                  OSKernel (fenêtres/multitâche), AppInstance, adaptateurs, stockage par app
  Desktop/               bureau, dock, fenêtres, switcher, réglages TriOS
  Modules/
    Henge/               interface + adaptateur sur henge_vm_* (C)
    Kernarium/           interface + adaptateur sur kernarium_runtime_loader_* (C)
    LinuxRunner/         interface + adaptateur sur LRRuntimeBridge (ObjC)
Vendor/                  les 3 codes sources d'origine, copiés par integrate.sh (non inclus ici)
Scripts/                 integrate.sh, build_engines.sh
Docs/                    ANALYSE.md, BUILD.md, PATCHES.md, INTEGRATION_*.md, schema.md
```

## Licences

Assemblage GPL : Henge GPL-2.0-or-later, Kernarium GPL-2.0-or-later,
LinuxRunner GPL-3.0. Le binaire combiné est donc GPL et ne peut pas
aller sur l'App Store — installation par sideload (SideStore,
AltStore, TrollStore), exactement comme les trois apps d'origine.
