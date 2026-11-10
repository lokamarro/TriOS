#!/bin/bash
# TriOS — construction SÉPARÉE des trois moteurs et génération de
# Config/Engines.xcconfig. Chaque moteur garde SON système de build
# d'origine ; ce script les enchaîne et refuse de continuer si un
# prérequis manque, en disant lequel.
set -uo pipefail
cd "$(dirname "$0")/.."
XCC="Config/Engines.xcconfig"
SWIFT_FLAGS=""; C_DEFS=""; HEADERS=""; LIBS=""
note() { echo; echo "== $* =="; }
die()  { echo "ARRÊT : $*"; echo "Corrige ce point puis relance le script (les moteurs déjà construits sont conservés)."; exit 1; }

note "Henge : libhengevm.a"
if [ -d Vendor/Henge ]; then
  : "${HUSK:=$HOME/Documents/Coding/Husk}"
  [ -d "$HUSK" ] || die "Checkout Husk introuvable (\$HUSK=$HUSK). Henge en dépend pour StikJIT/libslirp/glib — voir son README, section Building."
  ( cd Vendor/Henge && HUSK="$HUSK" ios/scripts/fetch_deps.sh && make ios-lib ) || die "Build Henge en échec (make ios-lib). Voir la sortie ci-dessus."
  SWIFT_FLAGS="$SWIFT_FLAGS -DHENGE_LINKED"; C_DEFS="$C_DEFS HENGE_LINKED=1"
  HEADERS="$HEADERS \$(SRCROOT)/TriOS/Modules/Henge/Native \$(SRCROOT)/Vendor/Henge/core \$(SRCROOT)/Vendor/Henge/host \$(SRCROOT)/Vendor/Henge/devices"
  LIBS="$LIBS \$(SRCROOT)/Vendor/Henge/build/ios"
else
  echo "Vendor/Henge absent — lance d'abord Scripts/integrate.sh. Henge ignoré."
fi

note "LinuxRunner : runtime iSH (Meson)"
if [ -d Vendor/LinuxRunner ]; then
  command -v meson >/dev/null && command -v ninja >/dev/null || die "meson/ninja manquants : brew install meson ninja cmake pkg-config ld.lld"
  ( cd Vendor/LinuxRunner && git submodule update --init --recursive --checkout -- deps/libapps deps/libarchive deps/glslang deps/spirv-cross ) || echo "  ! Sous-modules non initialisés (pas un clone git ?) — suivre Vendor/LinuxRunner/docs/BUILDING.md."
  echo "  Le cœur LinuxRunner se construit via leur meson.build + iSH.xcodeproj d'origine :"
  echo "  suivre Vendor/LinuxRunner/docs/BUILDING.md (variante interpréteur d'abord),"
  echo "  puis décommenter LINUXRUNNER_LINKED dans ce script une fois RuntimeBridge lié."
  # SWIFT_FLAGS="$SWIFT_FLAGS -DLINUXRUNNER_LINKED"; C_DEFS="$C_DEFS LINUXRUNNER_LINKED=1"
  HEADERS="$HEADERS \$(SRCROOT)/Vendor/LinuxRunner/app \$(SRCROOT)/Vendor/LinuxRunner/app/LinuxRunner"
else
  echo "Vendor/LinuxRunner absent — ignoré."
fi

note "Kernarium : XCFrameworks (CMake)"
if [ -d Vendor/Kernarium ]; then
  command -v cmake >/dev/null && command -v ninja >/dev/null || die "cmake/ninja manquants : brew install cmake ninja"
  ( cd Vendor/Kernarium && dependencies/prepare.sh --no-fetch ) || die "dependencies/prepare.sh en échec — leurs préfixes statiques privés sont requis (voir Vendor/Kernarium/AGENTS.md)."
  echo "  Puis : presets CMake iOS du repo, et vérifier .build/kernarium-xcframeworks."
  echo "  Décommenter KERNARIUM_LINKED une fois les XCFrameworks présents et le bundle noyau accepté copié."
  # SWIFT_FLAGS="$SWIFT_FLAGS -DKERNARIUM_LINKED"; C_DEFS="$C_DEFS KERNARIUM_LINKED=1"
  HEADERS="$HEADERS \$(SRCROOT)/Vendor/Kernarium/GuestHost/Sources \$(SRCROOT)/Vendor/Kernarium/Kernarium/Runtime"
else
  echo "Vendor/Kernarium absent — ignoré."
fi

note "Écriture de $XCC"
{
  echo "// Généré par Scripts/build_engines.sh — ne pas éditer à la main."
  [ -n "$SWIFT_FLAGS" ] && echo "OTHER_SWIFT_FLAGS = \$(inherited)$SWIFT_FLAGS"
  [ -n "$C_DEFS" ] && echo "GCC_PREPROCESSOR_DEFINITIONS = \$(inherited)$C_DEFS"
  [ -n "$HEADERS" ] && echo "HEADER_SEARCH_PATHS = \$(inherited)$HEADERS"
  [ -n "$LIBS" ] && echo "LIBRARY_SEARCH_PATHS = \$(inherited)$LIBS"
  true
} > "$XCC"
cat "$XCC"
echo
echo "Terminé. Rouvre TriOS.xcodeproj, recompile, et vérifie sur l'appareil"
echo "qu'un moteur lié démarre avant de lier le suivant (ordre conseillé :"
echo "LinuxRunner, Henge, Kernarium — Docs/BUILD.md, étape 2)."
