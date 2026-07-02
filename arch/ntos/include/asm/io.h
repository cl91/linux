#ifndef _ASM_NTOS_IO_H
#define _ASM_NTOS_IO_H

#include <linux/types.h>
#include <host_ops.h>

#define ioremap ioremap
#define iounmap iounmap
#define ioremap_wc ioremap_wc
#define ioremap_wt ioremap_wt
extern void __iomem *ioremap(phys_addr_t offset, size_t size);
extern void __iomem *ioremap_wc(phys_addr_t offset, size_t size);
extern void __iomem *ioremap_wt(phys_addr_t offset, size_t size);
extern void iounmap(volatile void __iomem *addr);

#ifdef CONFIG_TARGET_ARCH_X86
#define _inb _inb
#define _inw _inw
#define _inl _inl
#define _outb _outb
#define _outw _outw
#define _outl _outl

static inline u8 _inb(unsigned long addr)
{
	return LnxDrvImportTable.ReadIoPort8(addr);
}

static inline u16 _inw(unsigned long addr)
{
	return LnxDrvImportTable.ReadIoPort16(addr);
}

static inline u32 _inl(unsigned long addr)
{
	return LnxDrvImportTable.ReadIoPort32(addr);
}

static inline void _outb(u8 value, unsigned long addr)
{
	LnxDrvImportTable.WriteIoPort8(addr, value);
}

static inline void _outw(u16 value, unsigned long addr)
{
	LnxDrvImportTable.WriteIoPort16(addr, value);
}

static inline void _outl(u32 value, unsigned long addr)
{
	LnxDrvImportTable.WriteIoPort32(addr, value);
}

#define insb insb
#define insw insw
#define insl insl
#define outsb outsb
#define outsw outsw
#define outsl outsl

#define DEFINE_IOPORT_STRING_OPS_READ(name, type, op)		\
	static inline void name(unsigned long addr,		\
				void *_buf, unsigned int count)	\
	{							\
		type *buf = _buf;				\
		for (unsigned int i = 0; i < count; i++) {	\
			buf[i] = op(addr);			\
		}						\
	}

#define DEFINE_IOPORT_STRING_OPS_WRITE(name, type, op)		\
	static inline void name(unsigned long addr,		\
				const void *_buf,		\
				unsigned int count)		\
	{							\
		const type *buf = _buf;				\
		for (unsigned int i = 0; i < count; i++) {	\
			op(buf[i], addr);			\
		}						\
	}

DEFINE_IOPORT_STRING_OPS_READ(insb, u8, _inb);
DEFINE_IOPORT_STRING_OPS_READ(insw, u16, _inw);
DEFINE_IOPORT_STRING_OPS_READ(insl, u32, _inl);
DEFINE_IOPORT_STRING_OPS_WRITE(outsb, u8, _outb);
DEFINE_IOPORT_STRING_OPS_WRITE(outsw, u16, _outw);
DEFINE_IOPORT_STRING_OPS_WRITE(outsl, u32, _outl);
#endif

/* NT guarantees that the lowest 64KB of a virtual address space is never mapped,
 * so if a mapped IO address is below 0x10000, we treat it as an IO port address. */
#define PCI_IOBASE ((void __iomem *)0)
#define IO_SPACE_LIMIT 0xffff

#define DEFINE_IO_OPS_READ(ty, len, sz)					\
	static inline ty ioread##len(const volatile void __iomem *addr)	\
	{								\
		if ((unsigned long)addr <= IO_SPACE_LIMIT) {		\
			return _in##sz((unsigned long)addr);		\
		} else {						\
			return read##sz(addr);				\
		}							\
	}

#define DEFINE_IO_OPS_WRITE(ty, len, sz)				\
	static inline void iowrite##len(ty value,			\
					volatile void __iomem *addr)	\
	{								\
		if ((unsigned long)addr <= IO_SPACE_LIMIT) {		\
			_out##sz(value, (unsigned long)addr);		\
		} else {						\
			write##sz(value, addr);				\
		}							\
	}

#define ioread8 ioread8
#define ioread16 ioread16
#define ioread32 ioread32
#ifdef CONFIG_64BIT
#define ioread64 ioread64
#endif

#define iowrite8 iowrite8
#define iowrite16 iowrite16
#define iowrite32 iowrite32
#ifdef CONFIG_64BIT
#define iowrite64 iowrite64
#endif

#define ioread16be ioread16be
#define ioread32be ioread32be
#ifdef CONFIG_64BIT
#define ioread64be ioread64be
#endif

#define iowrite16be iowrite16be
#define iowrite32be iowrite32be
#ifdef CONFIG_64BIT
#define iowrite64be iowrite64be
#endif

#define ioread8_rep ioread8_rep
#define ioread16_rep ioread16_rep
#define ioread32_rep ioread32_rep
#ifdef CONFIG_64BIT
#define ioread64_rep ioread64_rep
#endif

#define iowrite8_rep iowrite8_rep
#define iowrite16_rep iowrite16_rep
#define iowrite32_rep iowrite32_rep
#ifdef CONFIG_64BIT
#define iowrite64_rep iowrite64_rep
#endif

#include <linux/mm.h>
#include <asm-generic/io.h>

static inline u64 _inq(unsigned long addr)
{
	u32 lo = _inl(addr);
        u32 hi = _inl(addr + 4);
        return ((u64)hi << 32) | lo;
}

static inline void _outq(u64 value, unsigned long addr)
{
	u32 lo = value;
	u32 hi = value >> 32;
	_outl(lo, addr);
        _outl(hi, addr + 4);
}

DEFINE_IO_OPS_READ(u8, 8, b);
DEFINE_IO_OPS_READ(u16, 16, w);
DEFINE_IO_OPS_READ(u32, 32, l);
#ifdef CONFIG_64BIT
DEFINE_IO_OPS_READ(u64, 64, q);
#endif

DEFINE_IO_OPS_WRITE(u8, 8, b);
DEFINE_IO_OPS_WRITE(u16, 16, w);
DEFINE_IO_OPS_WRITE(u32, 32, l);
#ifdef CONFIG_64BIT
DEFINE_IO_OPS_WRITE(u64, 64, q);
#endif

static inline u16 ioread16be(const volatile void __iomem *addr)
{
	return swab16(ioread16(addr));
}

static inline u32 ioread32be(const volatile void __iomem *addr)
{
	return swab32(ioread32(addr));
}

#ifdef CONFIG_64BIT
static inline u64 ioread64be(const volatile void __iomem *addr)
{
	return swab64(ioread64(addr));
}
#endif

static inline void iowrite16be(u16 value, void volatile __iomem *addr)
{
	iowrite16(swab16(value), addr);
}

static inline void iowrite32be(u32 value, volatile void __iomem *addr)
{
	iowrite32(swab32(value), addr);
}

#ifdef CONFIG_64BIT
static inline void iowrite64be(u64 value, volatile void __iomem *addr)
{
	iowrite64(swab64(value), addr);
}
#endif

#define DEFINE_IO_STRING_OPS_READ(ty, len, sz)				\
	static inline void ioread##len##_rep(const volatile void __iomem *addr, \
					     void *buffer,		\
					     unsigned int count)	\
	{								\
		if ((unsigned long)addr <= IO_SPACE_LIMIT) {		\
			ins##sz((unsigned long)addr, buffer, count);	\
		} else {						\
			reads##sz(addr, buffer, count);			\
		}							\
	}

#define DEFINE_IO_STRING_OPS_WRITE(ty, len, sz)				\
	static inline void iowrite##len##_rep(volatile void __iomem *addr, \
					      const void *buffer,	\
					      unsigned int count)	\
	{								\
		if ((unsigned long)addr <= IO_SPACE_LIMIT) {		\
			outs##sz((unsigned long)addr, buffer, count);	\
		} else {						\
			writes##sz(addr, buffer, count);		\
		}							\
	}

DEFINE_IO_STRING_OPS_READ(u8, 8, b);
DEFINE_IO_STRING_OPS_READ(u16, 16, w);
DEFINE_IO_STRING_OPS_READ(u32, 32, l);
#ifdef CONFIG_64BIT
DEFINE_IOPORT_STRING_OPS_READ(insq, u64, _inq);
DEFINE_IO_STRING_OPS_READ(u64, 64, q);
#endif

DEFINE_IO_STRING_OPS_WRITE(u8, 8, b);
DEFINE_IO_STRING_OPS_WRITE(u16, 16, w);
DEFINE_IO_STRING_OPS_WRITE(u32, 32, l);
#ifdef CONFIG_64BIT
DEFINE_IOPORT_STRING_OPS_WRITE(outsq, u64, _outq);
DEFINE_IO_STRING_OPS_WRITE(u64, 64, q);
#endif

#endif /* _ASM_NTOS_IO_H */

