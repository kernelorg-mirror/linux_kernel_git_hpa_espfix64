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
#include <linux/random.h>
#include <asm/pgtable.h>
#include <asm/setup.h>

/*
 * Note: we only need 6*8 = 48 bytes for the espfix stack, but round
 * it up to a cache line to avoid unnecessary sharing.
 */
#define ESPFIX_STACK_SIZE	(8*8UL)
#define ESPFIX_STACKS_PER_PAGE	(PAGE_SIZE/ESPFIX_STACK_SIZE)

/* There is address space for how many espfix pages? */
#define ESPFIX_PAGE_SPACE	(1UL << (PGDIR_SHIFT-PAGE_SHIFT-16))

#define ESPFIX_MAX_CPUS		(ESPFIX_STACKS_PER_PAGE * ESPFIX_PAGE_SPACE)
#if CONFIG_NR_CPUS > ESPFIX_MAX_CPUS
# error "Need more than one PGD for the ESPFIX hack"
#endif

#define ESPFIX_BASE_ADDR	(-2UL << PGDIR_SHIFT)

#define PGALLOC_GFP (GFP_KERNEL | __GFP_NOTRACK | __GFP_REPEAT | __GFP_ZERO)

/* This contains the *bottom* address of the espfix stack */
DEFINE_PER_CPU_READ_MOSTLY(unsigned long, espfix_stack);
DEFINE_PER_CPU_READ_MOSTLY(unsigned long, espfix_waddr);

/* Initialization mutex - should this be a spinlock? */
static DEFINE_MUTEX(espfix_init_mutex);

/* Page allocation bitmap - each page serves ESPFIX_STACKS_PER_PAGE CPUs */
#define ESPFIX_MAX_PAGES  DIV_ROUND_UP(CONFIG_NR_CPUS, ESPFIX_STACKS_PER_PAGE)
static void *espfix_pages[ESPFIX_MAX_PAGES];

static __page_aligned_bss pud_t espfix_pud_page[PTRS_PER_PUD]
	__aligned(PAGE_SIZE);

static unsigned int page_random, slot_random;

/*
 * This returns the bottom address of the espfix stack for a specific CPU.
 * The math allows for a non-power-of-two ESPFIX_STACK_SIZE, in which case
 * we have to account for some amount of padding at the end of each page.
 */
static inline unsigned long espfix_base_addr(unsigned int cpu)
{
	unsigned long page, slot;
	unsigned long addr;

	page = (cpu / ESPFIX_STACKS_PER_PAGE) ^ page_random;
	slot = (cpu + slot_random) % ESPFIX_STACKS_PER_PAGE;
	addr = (page << PAGE_SHIFT) + (slot * ESPFIX_STACK_SIZE);
	addr = (addr & 0xffffUL) | ((addr & ~0xffffUL) << 16);
	addr += ESPFIX_BASE_ADDR;
	return addr;
}

#define PTE_STRIDE        (65536/PAGE_SIZE)
#define ESPFIX_PTE_CLONES (PTRS_PER_PTE/PTE_STRIDE)
#define ESPFIX_PMD_CLONES PTRS_PER_PMD
#define ESPFIX_PUD_CLONES (65536/(ESPFIX_PTE_CLONES*ESPFIX_PMD_CLONES))

#define PGTABLE_PROT	  ((_KERNPG_TABLE & ~_PAGE_RW) | _PAGE_NX)

static void init_espfix_random(void)
{
	unsigned long rand;

	/*
	 * This is run before the entropy pools are initialized,
	 * but this is hopefully better than nothing.
	 */
	if (!arch_get_random_long(&rand)) {
		/* The constant is an arbitrary large prime */
		rdtscll(rand);
		rand *= 0xc345c6b72fd16123UL;
	}

	slot_random = rand % ESPFIX_STACKS_PER_PAGE;
	page_random = (rand / ESPFIX_STACKS_PER_PAGE)
		& (ESPFIX_PAGE_SPACE - 1);
}

void init_espfix_bsp(void)
{
	pgd_t pgd, *pgd_p;
	pteval_t ptemask;

	printk(KERN_INFO "init_espfix_bsp\n");

	ptemask = __supported_pte_mask;

	/* Install the espfix pud into the kernel page directory */
	pgd_p = &init_level4_pgt[pgd_index(ESPFIX_BASE_ADDR)];
	pgd = __pgd(__pa_symbol(espfix_pud_page) | (PGTABLE_PROT & ptemask));
	set_pgd(pgd_p, pgd);

	/* Randomize the locations */
	init_espfix_random();

	/* The rest is the same as for any other processor */
	init_espfix_ap();
}

void init_espfix_ap(void)
{
	unsigned int cpu, page;
	unsigned long addr;
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
	stack_page = ACCESS_ONCE(espfix_pages[page]);
	if (likely(stack_page))
		goto done;

	mutex_lock(&espfix_init_mutex);

	/* Did we race on the lock? */
	stack_page = ACCESS_ONCE(espfix_pages[page]);
	if (stack_page)
		goto unlock_done;

	ptemask = __supported_pte_mask;

	pud_p = &espfix_pud_page[pud_index(addr)];
	pud = *pud_p;
	if (!pud_present(pud)) {
		pmd_p = (pmd_t *)__get_free_page(PGALLOC_GFP);
		pud = __pud(__pa(pmd_p) | (PGTABLE_PROT & ptemask));
		for (n = 0; n < ESPFIX_PUD_CLONES; n++)
			set_pud(&pud_p[n], pud);
	}

	pmd_p = pmd_offset(&pud, addr);
	pmd = *pmd_p;
	if (!pmd_present(pmd)) {
		pte_p = (pte_t *)__get_free_page(PGALLOC_GFP);
		pmd = __pmd(__pa(pte_p) | (PGTABLE_PROT & ptemask));
		for (n = 0; n < ESPFIX_PMD_CLONES; n++)
			set_pmd(&pmd_p[n], pmd);
	}

	pte_p = pte_offset_kernel(&pmd, addr);
	stack_page = (void *)__get_free_page(GFP_KERNEL);
	pte = __pte(__pa(stack_page) | (__PAGE_KERNEL_RO & ptemask));
	for (n = 0; n < ESPFIX_PTE_CLONES; n++)
		set_pte(&pte_p[n*PTE_STRIDE], pte);

	/* Job is done for this CPU and any CPU which shares this page */
	ACCESS_ONCE(espfix_pages[page]) = stack_page;

unlock_done:
	mutex_unlock(&espfix_init_mutex);
done:
	this_cpu_write(espfix_stack, addr);
	this_cpu_write(espfix_waddr, (unsigned long)stack_page
		       + (addr & ~PAGE_MASK));

	printk(KERN_INFO "espfix: cpu %u stack %p waddr %p\n",
	       cpu, (void *)addr, (void *)this_cpu_read(espfix_waddr));
}
