// SPDX-License-Identifier: GPL-2.0
/*
 * idmap_handoff.c - final kexec hand-off.
 *
 * Target: ARMv7 (Cortex-A53, A32 state), 4.9.77, SMP/2, CONFIG_ARM_LPAE off
 * (classic 2-level short-descriptor page tables), CONFIG_OUTER_CACHE=y,
 * PAGE_OFFSET 0xC0000000, CONFIG_ARM_PATCH_PHYS_VIRT=y.
 *
 * This replicates the tail of the in-kernel machine_kexec()/__soft_restart()
 * path, which is NOT exported on this build (soft_restart, cpu_reset,
 * setup_mm_for_reboot, identity_mapping_add, phys_to_virt are all private).
 * We therefore build the identity map ourselves and flush caches with the
 * exported low-level primitives.
 *
 * The one entry point, kexec_handoff(), does:
 *   1. mask IRQ/FIQ on this (only) CPU
 *   2. build a private pgd = full copy of the live kernel pgd + identity
 *      sections for the control page and for kx_cpu_reset
 *   3. clean that pgd to RAM, install it in TTBR0, flush BP + TLB
 *   4. flush inner + outer caches to PoC, disable the outer cache
 *   5. jump to the *physical* (identity-mapped) address of kx_cpu_reset,
 *      which turns the MMU off and branches to the control page.
 *
 * References: arch/arm/kernel/machine_kexec.c,
 * arch/arm/kernel/reboot.c:__soft_restart(), arch/arm/mm/idmap.c
 * (identity_mapping_add / idmap_add_pmd), arch/arm/mm/mmu.c
 * (setup_mm_for_reboot).
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/string.h>
#include <linux/irqflags.h>

#include <asm/cacheflush.h>
#include <asm/outercache.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/cache.h>
#include <asm/memory.h>

/* ---- contract with the sibling relocate.S (do not redefine) -------------- */
extern void kx_relocate(void);
extern void kx_relocate_end(void);
extern void kx_cpu_reset(unsigned long phys) __attribute__((noreturn));

/*
 * v7_flush_kern_cache_all() is exported (see Module.symvers) but has no
 * prototype in any public header, so declare it here.
 */
extern void v7_flush_kern_cache_all(void);

/* Base mask of a 1 MB section descriptor: physical bits [31:20]. */
#ifndef SECTION_MASK
#define SECTION_MASK	(~(SECTION_SIZE - 1))
#endif

typedef void (*kx_reset_fn)(unsigned long);

/*
 * Resolve a kernel/module virtual address to its physical address by walking
 * the live kernel page tables (init_mm.pgd).
 *
 * virt_to_phys() cannot be used for kx_cpu_reset: module code lives in the
 * vmalloc/module region, not the linear map, so the PATCH_PHYS_VIRT linear
 * translation would return garbage.  A software walk is the only correct way.
 * Handles both a 1 MB section (unlikely for module text) and the normal
 * 4 KB page (table) case.
 */
static unsigned long km_va_to_pa(unsigned long va)
{
	pgd_t *pgd = pgd_offset_k(va);
	pud_t *pud = pud_offset(pgd, va);
	pmd_t *pmd = pmd_offset(pud, va);
	pte_t *pte;

	if ((pmd_val(*pmd) & PMD_TYPE_MASK) == PMD_TYPE_SECT)
		return (pmd_val(*pmd) & SECTION_MASK) | (va & ~SECTION_MASK);

	pte = pte_offset_kernel(pmd, va);
	return (pte_val(*pte) & PAGE_MASK) | (va & ~PAGE_MASK);
}

/*
 * Overlay a 1:1 (VA == PA) mapping for the 2 MB region that contains @phys
 * into @pgd_base.  This is a direct replica of the non-LPAE
 * arch/arm/mm/idmap.c:idmap_add_pmd(): a single Linux pgd slot is two 1 MB
 * hardware short-descriptor sections (pmd[0], pmd[1]); we align to the 2 MB
 * PMD boundary and write both halves.
 *
 * Descriptor bits are exactly those identity_mapping_add() applies to a
 * default (prot == 0) map:
 *      PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AF
 * i.e. an executable (XN clear), privileged read/write section.  PMD_SECT_AF
 * is 0 on non-LPAE.  The ARMv5-and-below PMD_BIT4 is intentionally omitted:
 * the target is ARMv7 (Cortex-A35), where upstream would not set it either.
 * With TEX=C=B=0 the section is a strongly-ordered / uncached mapping, matching
 * the kernel's own idmap — safe once we have cleaned everything to PoC.
 */
static void km_idmap_section(pgd_t *pgd_base, unsigned long phys)
{
	unsigned long base = phys & PMD_MASK;		/* 2 MB aligned */
	pgd_t *pgd = pgd_base + pgd_index(base);
	pmd_t *pmd = pmd_offset(pud_offset(pgd, base), base);
	unsigned long ent = (base & PMD_MASK) |
			    (PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AF);

	pmd[0] = __pmd(ent);
	pmd[1] = __pmd(ent + SECTION_SIZE);
}

/*
 * Clean a virtual range to the Point of Coherency (DCCMVAC), then a barrier.
 * flush_pmd_entry() is not exported, so this is how we push the freshly
 * written identity pgd out past both the inner and outer caches so the
 * hardware table walker sees it, whatever its walk attributes.
 */
static void km_clean_to_poc(const void *start, size_t size)
{
	unsigned long a = (unsigned long)start & ~(L1_CACHE_BYTES - 1);
	unsigned long end = (unsigned long)start + size;

	for (; a < end; a += L1_CACHE_BYTES)
		asm volatile("mcr p15, 0, %0, c7, c10, 1"	/* DCCMVAC */
			     : : "r" (a) : "memory");
	asm volatile("dsb" : : : "memory");
}

/*
 * Never returns.  reboot_code_phys = physical address of the copied
 * kx_relocate blob inside the control page.  Must be called with only ONE
 * CPU online, IRQs still enabled on entry (we disable them), caches still on
 * (we flush them).
 */
void kexec_handoff(unsigned long reboot_code_phys)
{
	unsigned long reset_va = (unsigned long)kx_cpu_reset;
	unsigned long reset_pa;
	unsigned long pgd_phys;
	unsigned long ttbr0;
	kx_reset_fn phys_reset;
	pgd_t *idpgd;

	/*
	 * Allocate the identity pgd while we can still sleep (caller has IRQs
	 * on).  order-2 == 16 KB, and the buddy allocator returns it aligned to
	 * 16 KB, exactly the alignment TTBR0 requires with non-LPAE / TTBCR.N=0.
	 */
	idpgd = (pgd_t *)__get_free_pages(GFP_KERNEL, 2);
	if (!idpgd) {
		pr_err("kexec_handoff: failed to allocate identity pgd\n");
		return;			/* nothing changed; caller aborts */
	}

	/*
	 * Full copy of the live kernel pgd (16 KB).  This keeps every kernel,
	 * vmalloc and module mapping valid after we switch TTBR0, so we can
	 * still run our own module code and call the exported cache/TLB helpers.
	 * The identity sections we add sit at pgd_index(phys) of RAM-physical
	 * addresses (< PAGE_OFFSET), which init_mm leaves empty, so they do not
	 * disturb the kernel half.
	 */
	memcpy(idpgd, init_mm.pgd, PTRS_PER_PGD * sizeof(pgd_t));

	reset_pa = km_va_to_pa(reset_va);
	pgd_phys = virt_to_phys(idpgd);

	/* The two things that must stay addressable 1:1 across MMU-off. */
	km_idmap_section(idpgd, reboot_code_phys);
	km_idmap_section(idpgd, reset_pa);

	/* All logging happens here, before we start tearing caches down. */
	pr_info("kexec_handoff: idpgd va=%p pa=%#lx\n", idpgd, pgd_phys);
	pr_info("kexec_handoff: control_phys=%#lx kx_cpu_reset va=%#lx pa=%#lx\n",
		reboot_code_phys, reset_va, reset_pa);

	/* ---- point of no return: no printk beyond here ---- */

	/* 1. Mask interrupts on this (the only) CPU. */
	local_irq_disable();
	local_fiq_disable();

	/*
	 * 2/3. Push the new page tables to RAM (the walker may read them
	 * uncached), then install them in TTBR0, preserving this CPU's existing
	 * page-table-walk attributes (cpu_switch_mm is not exported, so we do
	 * the CP15 writes ourselves).  Then invalidate the branch predictor and
	 * the whole TLB, matching setup_mm_for_reboot()'s cpu_switch_mm +
	 * local_flush_bp_all + local_flush_tlb_all.
	 */
	km_clean_to_poc(idpgd, PTRS_PER_PGD * sizeof(pgd_t));

	asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r" (ttbr0));  /* read TTBR0 */
	ttbr0 = (pgd_phys & 0xffffc000) |	/* new 16 KB-aligned base */
		(ttbr0 & 0x7f);			/* keep IRGN/S/RGN/NOS walk attrs */

	asm volatile("mcr p15, 0, %0, c13, c0, 1" : : "r" (0));	  /* CONTEXTIDR: reserved ASID */
	asm volatile("isb");
	asm volatile("mcr p15, 0, %0, c2, c0, 0" : : "r" (ttbr0)); /* write TTBR0 */
	asm volatile("isb");
	asm volatile("mcr p15, 0, %0, c7, c5, 6" : : "r" (0));	  /* BPIALL: flush branch pred */
	asm volatile("dsb");
	flush_tlb_all();

	/*
	 * 4. Flush caches to the Point of Coherency and turn the outer cache
	 * off.  Inner via the exported v7 set/way routine, then the outer
	 * (L2) cache; guard the optional function pointers.  kx_cpu_reset does
	 * the final SCTLR M/C-bit teardown when it turns the MMU off.
	 */
	v7_flush_kern_cache_all();
	if (outer_cache.flush_all)
		outer_cache.flush_all();
	if (outer_cache.disable)
		outer_cache.disable();

	/*
	 * 5. Enter kx_cpu_reset at its *physical* (identity-mapped) address, as
	 * __soft_restart() does with virt_to_idmap(cpu_reset): the PC must
	 * already be at a VA==PA location at the instant kx_cpu_reset turns the
	 * MMU off, or the next fetch would fault.  It never returns; it branches
	 * to reboot_code_phys (the kx_relocate blob) with the MMU off.
	 */
	phys_reset = (kx_reset_fn)reset_pa;
	phys_reset(reboot_code_phys);

	/* Unreachable. */
	for (;;)
		;
}
