/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_SLIRP_NET_H
#define UML_SLIRP_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The SLiRP user-mode network provider, as a launcher hands it to the
 * emulator's network operations. A context comes from uml_slirp_net_create
 * and is released by uml_slirp_net_destroy, after uml_slirp_net_stop. The
 * other calls return a negative value on failure. */
void *uml_slirp_net_create(void);
void uml_slirp_net_destroy(void *context);
int uml_slirp_net_add_forward(void *context, const char *specification);
int uml_slirp_net_open(void *context, unsigned char mac[6], unsigned int *mtu);
int uml_slirp_net_irq_fd(void *context);
int uml_slirp_net_read(void *context, void *buffer, size_t capacity);
int uml_slirp_net_write(void *context, const void *buffer, size_t length);
void uml_slirp_net_stop(void *context);

#ifdef __cplusplus
}
#endif

#endif
