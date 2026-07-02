/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_NTOS_DMA_H
#define _ASM_NTOS_DMA_H

#ifdef CONFIG_PCI
extern int isa_dma_bridge_buggy;
#else
#define isa_dma_bridge_buggy (0)
#endif

#define MAX_DMA_ADDRESS (~0UL)

#endif /* _ASM_NTOS_DMA_H */
