/* SPDX-License-Identifier: GPL-2.0 */
/*
 * relocate.S - kexec relocation + CPU reset trampoline
 *
 * Ported from the matching Linux 4.9.77 ARM tree:
 *   arch/arm/kernel/relocate_kernel.S  (relocate_new_kernel)
 *   arch/arm/mm/proc-v7.S              (cpu_v7_reset)
 *
 * Target: MediaTek MT8512/MT8110, Cortex-A53 in AArch32 / A32 state.
 *   CONFIG_ARM_LPAE=off (classic short-descriptor 2-level page tables),
 *   CONFIG_THUMB2_KERNEL=off (A32/ARM code), CONFIG_CPU_V7=y,
 *   CONFIG_OUTER_CACHE=y, PAGE_OFFSET=0xC0000000.
 *
 * Everything here is renamed with a kx_ prefix so it never clashes with the
 * in-kernel kexec_* / cpu_v7_reset symbols of the running (kexec-less) kernel.
 *
 * ---------------------------------------------------------------------------
 * REGISTER / DATA-WORD CONTRACT (the C loader relies on this verbatim)
 * ---------------------------------------------------------------------------
 *
 * kx_relocate is a position-independent blob. The C loader copies the whole
 * span [kx_relocate, kx_relocate_end) into the control page (fncpy-style),
 * then patches the four trailing DATA WORDS in the *copy* at these byte
 * offsets relative to the start of the copied blob:
 *
 *     kx_start_address    @ +0x78   new kernel entry, physical
 *     kx_indirection_page @ +0x7C   kexec indirection/relocation list, phys
 *                                   (0 => crashdump: skip relocation)
 *     kx_mach_type        @ +0x80   machine type (~0 for DT boot)
 *     kx_boot_atags       @ +0x84   DTB physical address (passed in r2)
 *
 *     kx_relocate_end - kx_relocate = 0x88 (136) bytes total.
 *
 * These offsets are also computable at build time from the exported labels:
 *   (&kx_start_address    - &kx_relocate),
 *   (&kx_indirection_page - &kx_relocate), etc.
 *
 * The loader turns off the MMU (via kx_cpu_reset) so that the copied blob is
 * entered identity-mapped at its physical address, then branches to the copied
 * kx_relocate. kx_relocate walks the indirection list, copies each source
 * page (1024 words) to its destination, and finally hands off to the new
 * kernel with the standard ARM boot register state:
 *
 *     r0 = 0
 *     r1 = kx_mach_type      (machine type; ~0 for a DT/DTB boot)
 *     r2 = kx_boot_atags     (physical address of the DTB / ATAGs)
 *     pc = kx_start_address  (new kernel entry, physical)
 *
 * kx_cpu_reset(unsigned long phys_entry):  called by C as
 *     void kx_cpu_reset(unsigned long phys_entry);
 * with r0 = phys_entry. Caches/MMU are assumed already flushed by the caller.
 * It clears SCTLR.M (MMU), SCTLR.C (D-cache) and SCTLR.I (I-cache), ISBs, and
 * branches (bx r0) to the physical entry. It runs identity-mapped, so PC stays
 * valid once the MMU is off.
 * ---------------------------------------------------------------------------
 */

	.text
	.arm

	.align	3	/* not needed for this code, but keeps fncpy() happy */

	.globl	kx_relocate
	.type	kx_relocate, %function
kx_relocate:

	ldr	r0,kx_indirection_page
	ldr	r1,kx_start_address

	/*
	 * If there is no indirection page (we are doing crashdumps)
	 * skip any relocation.
	 */
	cmp	r0, #0
	beq	2f

0:	/* top, read another word for the indirection page */
	ldr	r3, [r0],#4

	/* Is it a destination page. Put destination address to r4 */
	tst	r3,#1,0
	beq	1f
	bic	r4,r3,#1
	b	0b
1:
	/* Is it an indirection page */
	tst	r3,#2,0
	beq	1f
	bic	r0,r3,#2
	b	0b
1:

	/* are we done ? */
	tst	r3,#4,0
	beq	1f
	b	2f

1:
	/* is it source ? */
	tst	r3,#8,0
	beq	0b
	bic	r3,r3,#8
	mov	r6,#1024
9:
	ldr	r5,[r3],#4
	str	r5,[r4],#4
	subs	r6,r6,#1
	bne	9b
	b	0b

2:
	/* Jump to relocated kernel */
	mov	lr,r1
	mov	r0,#0
	ldr	r1,kx_mach_type
	ldr	r2,kx_boot_atags
	bx	lr			/* upstream: ARM( ret lr ) ; A32 => bx lr */

	.align

	.globl	kx_start_address
kx_start_address:
	.long	0x0

	.globl	kx_indirection_page
kx_indirection_page:
	.long	0x0

	.globl	kx_mach_type
kx_mach_type:
	.long	0x0

	/* phys addr of the atags/DTB for the new kernel */
	.globl	kx_boot_atags
kx_boot_atags:
	.long	0x0

	.size	kx_relocate, . - kx_relocate

	.globl	kx_relocate_end
kx_relocate_end:

/*
 * kx_cpu_reset - port of cpu_v7_reset (arch/arm/mm/proc-v7.S).
 *
 * void kx_cpu_reset(unsigned long phys_entry);   @ r0 = phys_entry
 *
 * MMU/caches are assumed already flushed by the caller. Upstream 4.9
 * cpu_v7_reset clears only SCTLR.M; per this module's contract we also clear
 * SCTLR.C and SCTLR.I so no cached/prefetched state survives into the new
 * kernel (safe because the caller has flushed). A32 build: the THUMB() TE-bit
 * clear is not needed. Runs identity-mapped, so PC stays valid after M=0.
 */
	.align	2
	.globl	kx_cpu_reset
	.type	kx_cpu_reset, %function
kx_cpu_reset:
	mrc	p15, 0, r1, c1, c0, 0		@ read SCTLR
	bic	r1, r1, #0x1			@ ...............m  clear M (MMU)
	bic	r1, r1, #(1 << 2)		@ clear C (D-cache enable)
	bic	r1, r1, #(1 << 12)		@ clear I (I-cache enable)
	mcr	p15, 0, r1, c1, c0, 0		@ disable MMU + caches
	isb
	bx	r0				@ branch to physical entry
	.size	kx_cpu_reset, . - kx_cpu_reset
