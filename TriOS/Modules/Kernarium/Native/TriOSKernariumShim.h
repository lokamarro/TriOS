
// Déclarations du loader Kernarium, recopiées de
// GuestHost/Sources/KernariumRuntimeLoader.h (GPL-2.0-or-later).
// Le loader d'origine inclut KernariumRuntime.h par un chemin relatif
// à SON arborescence (../../Kernarium/Runtime/...) ; dans TriOS les
// deux en-têtes sont copiés côte à côte par Scripts/integrate.sh,
// d'où ce shim qui déclare les fonctions utilisées par l'adaptateur.
#ifndef TRIOS_KERNARIUM_SHIM_H
#define TRIOS_KERNARIUM_SHIM_H

#import <Foundation/Foundation.h>
#import <MetalKit/MetalKit.h>
#include "KernariumRuntime.h"

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
extern "C" {
#endif

/* Choisit et charge un runtime pour la vie du processus. */
int kernarium_runtime_loader_select(uint32_t guest_flavor);
/* Démarre le guest sur la vue ; BLOQUANT, revient quand le guest s'arrête. */
int kernarium_runtime_loader_run(MTKView* view, const kernarium_runtime_boot* boot);
void kernarium_runtime_loader_request_stop(void);
int kernarium_runtime_loader_sync(void);
int kernarium_runtime_loader_quiesce(void);
int kernarium_runtime_loader_quiesce_resident(void);
void kernarium_runtime_loader_resume(void);
int kernarium_runtime_loader_set_background_throttle(int enabled);

#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END

#endif
