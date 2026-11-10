#!/bin/bash
# TriOS — intégration des trois codes sources d'origine dans Vendor/
# Usage : ./Scripts/integrate.sh Henge-0.2.zip Kernarium-main.zip LinuxRunner-main.zip
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 3 ] || { echo "Usage: $0 <Henge.zip> <Kernarium.zip> <LinuxRunner.zip>"; exit 1; }
mkdir -p Vendor TriOS/Modules/Henge/Native TriOS/Modules/Kernarium/Native TriOS/Modules/LinuxRunner/Native

echo "== Décompression =="
rm -rf Vendor/Henge Vendor/Kernarium Vendor/LinuxRunner
mkdir -p Vendor/Henge Vendor/Kernarium Vendor/LinuxRunner
unzip -q "$1" -d Vendor/Henge
unzip -q "$2" -d Vendor/Kernarium
unzip -q "$3" -d Vendor/LinuxRunner
for d in Henge Kernarium LinuxRunner; do
  count=$(find "Vendor/$d" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')
  if [ "$count" = "1" ]; then
    inner=$(find "Vendor/$d" -mindepth 1 -maxdepth 1 -type d)
    if [ -n "$inner" ]; then
      mv "$inner"/* "Vendor/$d"/ 2>/dev/null || true
      mv "$inner"/.[!.]* "Vendor/$d"/ 2>/dev/null || true
      rmdir "$inner" || true
    fi
  fi
done

echo "== En-têtes publics vers les modules TriOS =="
cp Vendor/Henge/ios/Henge/Native/henge-vm.h      TriOS/Modules/Henge/Native/ 2>/dev/null || echo "  ! henge-vm.h introuvable, vérifier Vendor/Henge/ios/"
cp Vendor/Henge/ios/Henge/Native/henge-ios-jit.h TriOS/Modules/Henge/Native/ 2>/dev/null || true
cp Vendor/Henge/ios/Henge/HengeBridge.h          TriOS/Modules/Henge/Native/ 2>/dev/null || true
cp Vendor/Kernarium/GuestHost/Sources/KernariumRuntimeLoader.h TriOS/Modules/Kernarium/Native/ 2>/dev/null || echo "  ! KernariumRuntimeLoader.h introuvable, vérifier Vendor/Kernarium/GuestHost/"
cp Vendor/Kernarium/Kernarium/Runtime/KernariumRuntime.h TriOS/Modules/Kernarium/Native/ 2>/dev/null || echo "  ! KernariumRuntime.h introuvable, vérifier Vendor/Kernarium/Kernarium/Runtime/"
cp Vendor/LinuxRunner/app/LinuxRunner/RuntimeBridge.h TriOS/Modules/LinuxRunner/Native/ 2>/dev/null || echo "  ! RuntimeBridge.h introuvable, vérifier Vendor/LinuxRunner/app/"

echo "== Neutralisation des points d'entrée (Docs/PATCHES.md §1) =="
for f in "Vendor/Henge/ios/Henge/HengeApp.swift" "Vendor/Kernarium/Kernarium/Kernarium/KernariumApp.swift"; do
  if [ -f "$f" ]; then
    sed -i '' 's/^@main/\/\/ @main désactivé par TriOS (voir Docs\/PATCHES.md)/' "$f" 2>/dev/null || sed -i 's/^@main/\/\/ @main désactivé par TriOS/' "$f"
    echo "  @main neutralisé : $f"
  fi
done
echo "  LinuxRunner : ne pas ajouter app/AppDelegate.m ni app/main.m au target TriOS (voir Docs/INTEGRATION_LINUXRUNNER.md)."
echo
echo "Intégration terminée. Étapes suivantes :"
echo "  1. ./Scripts/build_engines.sh   (long — lire Docs/BUILD.md d'abord)"
echo "  2. open TriOS.xcodeproj"
