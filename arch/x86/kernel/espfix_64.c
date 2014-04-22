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

#define ESPFIX_STACK_SIZE	64UL
#define ESPFIX_STACKS_PER_PAGE	(PAGE_SIZE/ESPFIX_STACK_SIZE)

#define ESPFIX_MAX_CPUS (ESPFIX_STACKS_PER_PAGE << (PGDIR_SHIFT-PAGE_SHIFT-16))
#if CONFIG_NR_CPUS > ESPFIX_MAX_CPUS
# error "Need more than one PGD for the ESPFIX hack"
#endif

#define ESPFIX_BASE_ADDR	(-2UL << PGDIR_SHIFT)

#define PGALLOC_GFP (GFP_KERNEL | __GFP_NOTRACK | __GFP_REPEAT | __GFP_ZERO)

/* This contains the *bottom* address of the espfix stack */
DEFINE_PER_CPU_READ_MOSTLY(unsigned long, espfix_stack);

/* Initialization mutex - should this be a spinlock? */
static DEFINE_MUTEX(espfix_init_mutex);

/* Page allocation bitmap - each page serves ESPFIX_STACKS_PER_PAGE CPUs */
#define ESPFIX_MAX_PAGES  DIV_ROUND_UP(CONFIG_NR_CPUS, ESPFIX_STACKS_PER_PAGE)
#define ESPFIX_MAP_SIZE   DIV_ROUND_UP(ESPFIX_MAX_PAGES, BITS_PER_LONG)
static unsigned long espfix_page_alloc_map[ESPFIX_MAP_SIZE];

static __page_aligned_bss pud_t espfix_pud_page[PTRS_PER_PUD]
	__aligned(PAGE_SIZE);

/*
 * This returns the bottom address of the espfix stack for a specific CPU.
 * The math allows for a non-power-of-two ESPFIX_STACK_SIZE, in which case
 * we have to account for some amount of padding at the end of each page.
 */
static inline unsigned long espfix_base_addr(unsigned int cpu)
{
	unsigned long page, addr;

	page = (cpu / ESPFIX_STACKS_PER_PAGE) << PAGE_SHIFT;
	addr = page + (cpu % ESPFIX_STACKS_PER_PAGE) * ESPFIX_STACK_SIZE;
	addr = (addr & 0xffffUL) | ((addr & ~0xffffUL) << 16);
	addr += ESPFIX_BASE_ADDR;
	return addr;
}

#define PTE_STRIDE        (65536/PAGE_SIZE)
#define ESPFIX_PTE_CLONES (PTRS_PER_PTE/PTE_STRIDE)
#define ESPFIX_PMD_CLONES PTRS_PER_PMD
#define ESPFIX_PUD_CLONES (65536/(ESPFIX_PTE_CLONES*ESPFIX_PMD_CLONES))

void init_espfix_this_cpu(void)
{
	unsigned int cpu, page;
	unsigned long addr;
	pgd_t pgd, *pgd_p;
	pud_t pud, *pud_p;
	pmd_t pmd, *pmd_p;
	pte_t pte, *pte_p;
	int n;
	void *stack_page;
	pteval_t ptemask;

	/* We only have to do this once... */
	if (likely(this_cpu_read(espfix_stack)))
		return;		/* Already initialized */

	cpu = smp_processor_id();
	addr = espfix_base_addr(cpu);
	page = cpu/ESPFIX_STACKS_PER_PAGE;

	/* Did another CPU already set this up? */
	if (likely(test_bit(page, espfix_page_alloc_map)))
		goto done;

	mutex_lock(&espfix_init_mutex);

	/* Did we race on the lock? */
	if (unlikely(test_bit(page, espfix_page_alloc_map)))
		goto unlock_done;

	ptemask = __supported_pte_mask;

	pgd_p = &init_level4_pgt[pgd_index(addr)];
	pgd = *pgd_p;
	if (!pgd_present(pgd)) {
		/* This can only happen on the BSP */
		/* XXX: is this early enough or will we have cloning issues? */
		pgd = __pgd(__pa_symbol(espfix_pud_page) |
			    (_KERNPG_TABLE & ptemask));
		set_pgd(pgd_p, pgd);
	}

	pud_p = &espfix_pud_page[pud_index(addr)];
	pud = *pud_p;
	if (!pud_present(pud)) {
		pmd_p = (pmd_t *)__get_free_page(PGALLOC_GFP);
		pud = __pud(__pa(pmd_p) | (_KERNPG_TABLE & ptemask));
		for (n = 0; n < ESPFIX_PUD_CLONES; n++)
			set_pud(&pud_p[n], pud);
	}

	pmd_p = pmd_offset(&pud, addr);
	pmd = *pmd_p;
	if (!pmd_present(pmd)) {
		pte_p = (pte_t *)__get_free_page(PGALLOC_GFP);
		pmd = __pmd(__pa(pte_p) | (_KERNPG_TABLE & ptemask));
		for (n = 0; n < ESPFIX_PMD_CLONES; n++)
			set_pmd(&pmd_p[n], pmd);
	}

	pte_p = pte_offset_kernel(&pmd, addr);
	stack_page = (void *)__get_free_page(GFP_KERNEL);
	pte = __pte(__pa(stack_page) | (__PAGE_KERNEL & ptemask));
	for (n = 0; n < ESPFIX_PTE_CLONES; n++)
		set_pte(&pte_p[n*PTE_STRIDE], pte);

	/* Job is done for this CPU and any CPU which shares this page */
	set_bit(page, espfix_page_alloc_map);

unlock_done:
	mutex_unlock(&espfix_init_mutex);
done:
	this_cpu_write(espfix_stack, addr);
}
