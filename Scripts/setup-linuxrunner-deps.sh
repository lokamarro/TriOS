#!/bin/bash
# TriOS — peuple les sous-modules LinuxRunner aux révisions épinglées.
#
# Contexte : Vendor/LinuxRunner est une copie de sources (pas un clone git),
# donc « git submodule update » ne peut pas fonctionner : les dossiers
# deps/libapps, deps/libarchive, deps/glslang et deps/spirv-cross sont vides.
# Ce script les remplit un par un, au commit exact indiqué dans
# Vendor/LinuxRunner/SOURCE_ORIGIN.txt, puis vérifie le résultat et lance
# le contrôle de prérequis d'origine (scripts/bootstrap-macos.sh, macOS).
#
# Usage :
#   Scripts/setup-linuxrunner-deps.sh           peuple + vérifie + bootstrap
#   Scripts/setup-linuxrunner-deps.sh --check   état seulement, ne change rien
#
# Idempotent : un sous-module déjà en place à la bonne révision est ignoré.
# deps/linux est volontairement ignoré (optionnel, update=none dans .gitmodules).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LR="$ROOT/Vendor/LinuxRunner"
DEPS_DIR="$LR/deps"
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

# nom|url|révision épinglée (SOURCE_ORIGIN.txt)
DEPS=(
  "libapps|https://github.com/ish-app/libapps|b8cacae35e5b11d64bb736a053921c16ca7faf9e"
  "libarchive|https://github.com/libarchive/libarchive|fc6563f5130d8a7ee1fc27c0e55baef35119f26c"
  "glslang|https://github.com/KhronosGroup/glslang.git|a57276bf558f5cf94d3a9854ebdf5a2236849a5a"
  "spirv-cross|https://github.com/KhronosGroup/SPIRV-Cross.git|2c32b6bf86f3c4a5539aa1f0bacbd59fe61759cf"
)

if [ ! -d "$LR" ]; then
  echo "ERREUR : $LR introuvable — lance d'abord Scripts/integrate.sh." >&2
  exit 1
fi

statut_dep() { # $1=dossier $2=sha attendu → 0 si en place à la bonne révision
  local dir="$1" sha="$2" head
  [ -d "$dir/.git" ] || return 1
  head="$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)"
  [ "$head" = "$sha" ]
}

peuple_dep() { # $1=nom $2=url $3=sha
  local nom="$1" url="$2" sha="$3" dir="$DEPS_DIR/$1"
  if statut_dep "$dir" "$sha"; then
    echo "  OK      $nom déjà en place ($sha)"
    return 0
  fi
  if [ -d "$dir" ] && [ -n "$(ls -A "$dir" 2>/dev/null)" ] && [ ! -d "$dir/.git" ]; then
    echo "  ERREUR  $nom : dossier non vide mais pas un dépôt git — déplace-le à la main puis relance." >&2
    return 1
  fi
  rm -rf "$dir"
  mkdir -p "$dir"
  echo "  …       $nom : récupération de $sha"
  # Récupération ciblée du commit épinglé (léger) ; repli sur un clone complet.
  if git -C "$dir" init -q \
     && git -C "$dir" remote add origin "$url" \
     && git -C "$dir" fetch -q --depth 1 origin "$sha" \
     && git -C "$dir" checkout -q --detach FETCH_HEAD; then
    :
  else
    echo "  …       $nom : repli sur un clone complet"
    rm -rf "$dir"
    git clone -q "$url" "$dir"
    git -C "$dir" checkout -q --detach "$sha"
  fi
  statut_dep "$dir" "$sha" && echo "  OK      $nom en place" || { echo "  ERREUR  $nom : révision inattendue après récupération." >&2; return 1; }
}

echo "LinuxRunner — sous-modules (dépôt : $LR)"
manquants=0
for entree in "${DEPS[@]}"; do
  IFS='|' read -r nom url sha <<< "$entree"
  if [ "$CHECK_ONLY" -eq 1 ]; then
    if statut_dep "$DEPS_DIR/$nom" "$sha"; then
      echo "  OK      $nom"
    else
      echo "  MANQUANT $nom (attendu : $sha)"
      manquants=1
    fi
  else
    peuple_dep "$nom" "$url" "$sha" || manquants=1
  fi
done

if [ "$CHECK_ONLY" -eq 1 ]; then
  [ "$manquants" -eq 0 ] && echo "Tout est en place." || echo "Lance le script sans --check pour peupler les sous-modules."
  exit "$manquants"
fi

if [ "$manquants" -ne 0 ]; then
  echo "Terminé avec des erreurs — voir ci-dessus." >&2
  exit 1
fi

# bootstrap-macos.sh contrôle les sous-modules via « git submodule status »
# exécuté dans Vendor/LinuxRunner. Comme c'est une copie et non un clone,
# git y remonte au dépôt parent et le contrôle échoue sur les pathspecs.
# On crée donc un dépôt git local minimal : un .gitmodules décrivant les
# quatre dépendances et des gitlinks pointant sur les dépôts déjà peuplés
# aux révisions épinglées — le contrôle d'origine vérifie alors réellement
# les révisions au lieu d'échouer sur la forme du dépôt.
if [ ! -e "$LR/.git" ]; then
  echo "Création d'un dépôt git local dans Vendor/LinuxRunner (contrôle bootstrap) :"
  git -C "$LR" init -q
  : > "$LR/.gitmodules"
  for entree in "${DEPS[@]}"; do
    IFS='|' read -r nom url sha <<< "$entree"
    git -C "$LR" config -f .gitmodules "submodule.deps/$nom.path" "deps/$nom"
    git -C "$LR" config -f .gitmodules "submodule.deps/$nom.url" "$url"
    git -C "$LR" add "deps/$nom"
  done
  git -C "$LR" submodule init >/dev/null 2>&1 || true
  git -C "$LR" submodule status --recursive -- deps/libapps deps/libarchive deps/glslang deps/spirv-cross | sed 's/^/  /'
fi

echo "Vérification des prérequis (bootstrap-macos.sh d'origine) :"
if [ "$(uname -s)" = "Darwin" ]; then
  bash "$LR/scripts/bootstrap-macos.sh"
else
  echo "  (non macOS : bootstrap-macos.sh sera à lancer sur le Mac — il vérifie Xcode, meson, ninja, ld.lld…)"
fi
echo "Fait. Prochaines étapes : Scripts/build_engines.sh, puis la liaison LinuxRunner (voir Docs/INTEGRATION_LINUXRUNNER.md)."
