//
//  TriOS-Bridging-Header.h
//  Les en-têtes natifs ne sont importés que si le moteur correspondant
//  est lié. Les macros HENGE_LINKED / KERNARIUM_LINKED /
//  LINUXRUNNER_LINKED doivent être définies à la fois :
//    - pour Swift : OTHER_SWIFT_FLAGS = -DHENGE_LINKED ...
//    - pour Clang : GCC_PREPROCESSOR_DEFINITIONS += HENGE_LINKED=1
//  Scripts/build_engines.sh écrit ces réglages dans
//  Config/Engines.xcconfig, déjà référencé par le projet.
//
#ifndef TRIOS_BRIDGING_HEADER_H
#define TRIOS_BRIDGING_HEADER_H

#ifdef HENGE_LINKED
// Copié par Scripts/integrate.sh depuis Vendor/Henge/ios/Henge/
#import "henge-vm.h"
#import "henge-ios-jit.h"
#endif

#ifdef KERNARIUM_LINKED
// Copiés par Scripts/integrate.sh depuis Vendor/Kernarium/ :
// KernariumRuntime.h (Runtime/) + le shim TriOS qui déclare les
// fonctions du loader (le loader d'origine s'inclut par un chemin
// relatif propre à son arborescence).
#import "TriOSKernariumShim.h"
#endif

#ifdef LINUXRUNNER_LINKED
// Copié par Scripts/integrate.sh depuis Vendor/LinuxRunner/app/LinuxRunner/
#import "RuntimeBridge.h"
#endif

#endif
