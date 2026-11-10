# Compilation pas à pas

## Étape 0 — Prérequis Mac

- macOS récent, **Xcode 16 ou plus (26 recommandé** : Henge demande
  Xcode 27 pour son propre build complet, LinuxRunner a été archivé
  avec le SDK iPhoneOS 27.2 ; TriOS lui-même se contente de 16+)
- `brew install xcodegen meson ninja cmake pkg-config ld.lld git python3`
  (xcodegen seulement si tu veux régénérer le projet depuis `project.yml`)
- Un iPhone/iPad arm64 (A14/M1 ou mieux conseillé), iOS 17+
- Pour le JIT : StikDebug, ou TrollStore (≤ iOS 17.0), ou jailbreak

## Étape 1 — Interface seule (vérifie que tout compile chez toi)

1. `open TriOS.xcodeproj`
2. Cible TriOS → Signing & Capabilities → choisis ton équipe,
   bundle ID libre (défaut `fr.trios.unified`).
3. Si la signature refuse les entitlements mémoire de
   `TriOS/TriOS.entitlements` (certificat gratuit), retire les deux
   clés `com.apple.developer.kernel.*` du fichier. L'app compile et
   tourne pareil, avec des plafonds mémoire iOS par défaut.
4. Run sur appareil. Tu dois voir le bureau TriOS, les 3 icônes,
   le dock, le switcher, et dans chaque app le bandeau orange
   « Moteur non lié ». **Si cette étape échoue, ne passe pas à la 2.**

## Étape 2 — Lier les moteurs (le gros du travail)

```bash
./Scripts/integrate.sh /chemin/Henge-0.2.zip /chemin/Kernarium-main.zip /chemin/LinuxRunner-main.zip
./Scripts/build_engines.sh
```

`integrate.sh` décompresse les trois sources dans `Vendor/`, copie
les en-têtes publics dans `TriOS/Modules/*/Native/` et applique les
neutralisations listées dans `Docs/PATCHES.md` (points d'entrée).

`build_engines.sh` construit chaque moteur **séparément** et écrit
`Config/Engines.xcconfig` (flags `-D*_LINKED`, chemins de recherche) :

1. **Henge** : exige un checkout Husk (`export HUSK=…`) →
   `ios/scripts/fetch_deps.sh`, `make ios-lib` → `libhengevm.a` +
   `libhengerestore.a`. Compte le téléchargement des dépendances.
2. **Kernarium** : `dependencies/prepare.sh` puis les presets CMake
   du repo (`CMakePresets.json`) → XCFrameworks dans
   `.build/kernarium-xcframeworks` + bundle noyau accepté.
   C'est l'étape la plus longue et la plus stricte (préfixes privés,
   aucun recours à Homebrew dans leur build).
3. **LinuxRunner** : sous-modules + Meson (voir leur
   `docs/BUILDING.md`) ; variante GPU seulement si tu as préparé
   ANGLE/virglrenderer aux révisions épinglées, sinon commence par
   la variante interpréteur/logicielle, fonctionnelle.

Ensuite, rouvre Xcode et recompile : les adaptateurs passent en mode
lié un par un. **Lie-les dans cet ordre : LinuxRunner, Henge,
Kernarium** (du plus autonome au plus exigeant), en vérifiant un
démarrage réel après chaque liaison.

## Étape 3 — Câblages restants, moteur par moteur

Deux adaptateurs gardent volontairement un point de branchement
explicite plutôt qu'un appel inventé :

- Kernarium : la `MTKView` à passer à
  `kernarium_runtime_loader_run` est celle de `GuestDisplayView`
  d'origine → `Docs/INTEGRATION_KERNARIUM.md`.
- LinuxRunner : l'appel `LRRuntimeBridge.startRuntime()` suppose
  l'`AppDelegate` iSH initialisé (fenêtre terminal d'origine) →
  `Docs/INTEGRATION_LINUXRUNNER.md` explique comment le rejouer
  dans `EngineBootstrap`.

Henge est câblé directement (`henge_vm_boot`, API sans vue au boot ;
la `CAMetalLayer` se branche à l'ouverture de l'onglet Session).

## Dépannage

| Symptôme | Cause probable |
|---|---|
| `duplicate symbol` à l'édition de liens | Deux moteurs liés « à plat » : reconstruire avec visibilité cachée / préfixage (ANALYSE.md §3) |
| L'app est tuée dès le boot d'un 2e moteur | Jetsam : baisse la RAM guest Henge à 4 Go, suspends Kernarium, vérifie l'entitlement mémoire |
| Henge : écran noir, aucun boot | JIT absent : StikDebug/TrollStore requis avant le lancement |
| Kernarium refuse un 2e `select` | Normal : flavor figé par processus, relance TriOS pour changer Linux/Android |
| Signature : entitlement refusé | Certificat sans la capacité : retire la clé concernée (étape 1.3) |
