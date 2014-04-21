/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2014 Intel Corporation; author: H. Peter Anvin
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2 or (at your
 *   option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/gfp.h>
#include <asm/pgtable.h>

#define ESPFIX_STACK_SIZE	64
#define ESPFIX_BASE_ADDR	(-2ULL << PGDIR_SHIFT)

#if CONFIG_NR_CPUS >= (8 << 20)/ESPFIX_STACK_SIZE
# error "Need more than one PGD for the ESPFIX hack"
#endif

#define PGALLOC_GFP (GFP_KERNEL | __GFP_NOTRACK | __GFP_REPEAT | __GFP_ZERO)
#define ESPFIX_PGD_FLAGS (__PAGE_KERNEL & ~_PAGE_DIRTY)
#define ESPFIX_PUD_FLAGS (__PAGE_KERNEL & ~_PAGE_DIRTY)
#define ESPFIX_PMD_FLAGS (__PAGE_KERNEL & ~_PAGE_DIRTY)
#define ESPFIX_PTE_FLAGS __PAGE_KERNEL

/* This contains the *bottom* address of the espfix stack */
DEFINE_PER_CPU_READ_MOSTLY(unsigned long, espfix_stack);

/* Initialization mutex - should this be a spinlock? */
static DEFINE_MUTEX(espfix_init_mutex);

static __page_aligned_bss pud_t espfix_pud_page[PTRS_PER_PUD]
	__aligned(PAGE_SIZE);

/* This returns the bottom address of the espfix stack for a specific CPU */
static inline unsigned long espfix_base_addr(int cpu)
{
	unsigned long addr = cpu * ESPFIX_STACK_SIZE;

	addr = (addr & 0xffffUL) | ((addr & ~0xffffUL) << 16);
	addr += ESPFIX_BASE_ADDR;
	return addr;
}

#define PTE_STRIDE        (65536/PAGE_SIZE)
#define ESPFIX_PTE_CLONES (PTRS_PER_PTE/PTE_STRIDE)
#define ESPFIX_PMD_CLONES PTRS_PER_PMD
#define ESPFIX_PUD_CLONES (65536/(ESPFIX_PTE_CLONES*ESPFIX_PMD_CLONES))

/*
 * Check to see if the espfix stuff is already installed.
 * We do this once before grabbing the lock and, if we have to,
 * once after.
 */
static bool espfix_already_there(unsigned long addr)
{
	const pgd_t *pgd_p;
	pgd_t pgd;
	const pud_t *pud_p;
	pud_t pud;
	const pmd_t *pmd_p;
	pmd_t pmd;
	const pte_t *pte_p;
	pte_t pte;
	int n;

	pgd_p = &init_level4_pgt[pgd_index(addr)];
	pgd = ACCESS_ONCE(*pgd_p);
	if (!pgd_present(pgd))
		return false;

	pud_p = &espfix_pud_page[pud_index(addr)];
	for (n = 0; n < ESPFIX_PUD_CLONES; n++) {
		pud = ACCESS_ONCE(pud_p[n]);
		if (!pud_present(pud))
			return false;
	}

	pmd_p = pmd_offset(&pud, addr);
	for (n = 0; n < ESPFIX_PMD_CLONES; n++) {
		pmd = ACCESS_ONCE(pmd_p[n]);
		if (!pmd_present(pmd))
			return false;
	}

	pte_p = pte_offset_kernel(&pmd, addr);
	for (n = 0; n < ESPFIX_PTE_CLONES; n++) {
		pte = ACCESS_ONCE(pte_p[n*PTE_STRIDE]);
		if (!pte_present(pte))
			return false;
	}

	return true;		/* All aliases present and accounted for */
}

void init_espfix_cpu(void)
{
	int cpu = smp_processor_id();
	unsigned long addr;
	pgd_t pgd, *pgd_p;
	pud_t pud, *pud_p;
	pmd_t pmd, *pmd_p;
	pte_t pte, *pte_p;
	int n;
	void *stack_page;

	cpu = smp_processor_id();
	BUG_ON(cpu >= (8 << 20)/ESPFIX_STACK_SIZE);

	/* We only have to do this once... */
	if (likely(this_cpu_read(espfix_stack)))
		return;		/* Already initialized */

	addr = espfix_base_addr(cpu);

	/* Did another CPU already set this up? */
	if (likely(espfix_already_there(addr)))
		goto done;

	mutex_lock(&espfix_init_mutex);

	if (unlikely(espfix_already_there(addr)))
		goto unlock_done;

	pgd_p = &init_level4_pgt[pgd_index(addr)];
	pgd = *pgd_p;
	if (!pgd_present(pgd)) {
		/* This can only happen on the BSP */
		pgd = __pgd(__pa(espfix_pud_page) |
			    (ESPFIX_PGD_FLAGS & __supported_pte_mask));
		set_pgd(pgd_p, pgd);
	}

	pud_p = &espfix_pud_page[pud_index(addr)];
	pud = *pud_p;
	if (!pud_present(pud)) {
		pmd_p = (pmd_t *)__get_free_page(PGALLOC_GFP);
		pud = __pud(__pa(pmd_p) |
			    (ESPFIX_PUD_FLAGS & __supported_pte_mask));
		for (n = 0; n < ESPFIX_PUD_CLONES; n++)
			set_pud(&pud_p[n], pud);
	}

	pmd_p = pmd_offset(&pud, addr);
	pmd = *pmd_p;
	if (!pmd_present(pmd)) {
		pte_p = (pte_t *)__get_free_page(PGALLOC_GFP);
		pmd = __pmd(__pa(pte_p) |
			    (ESPFIX_PMD_FLAGS & __supported_pte_mask));
		for (n = 0; n < ESPFIX_PMD_CLONES; n++)
			set_pmd(&pmd_p[n], pmd);
	}

	pte_p = pte_offset_kernel(&pmd, addr);
	stack_page = (void *)__get_free_page(GFP_KERNEL);
	pte = __pte(__pa(stack_page) |
		    (ESPFIX_PTE_FLAGS & __supported_pte_mask));
	for (n = 0; n < ESPFIX_PTE_CLONES; n++)
		set_pte(&pte_p[n*PTE_STRIDE], pte);

unlock_done:
	mutex_unlock(&espfix_init_mutex);
done:
	this_cpu_write(espfix_stack, addr);
	printk(KERN_ERR "espfix: Initializing espfix for cpu %d, stack @ %p\n",
		 cpu, (const void *)addr);
}
