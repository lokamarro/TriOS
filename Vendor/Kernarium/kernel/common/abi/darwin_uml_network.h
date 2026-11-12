/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_NETWORK_ABI_H
#define UML_DARWIN_NETWORK_ABI_H

/* The largest Ethernet frame the bridge carries.  It sizes the kernel's
 * receive buffer and each slot of the SLiRP provider's receive and transmit
 * rings, so the provider never holds a frame the kernel cannot take. */
#define DARWIN_UML_NET_FRAME_SIZE 2048

#ifdef __cplusplus
extern "C" {
#endif

int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu);
int darwin_uml_net_irq_fd(void);
int darwin_uml_net_read(void *buffer, unsigned long capacity);
int darwin_uml_net_write(const void *buffer, unsigned long length);

#ifdef __cplusplus
}
#endif

#endif
