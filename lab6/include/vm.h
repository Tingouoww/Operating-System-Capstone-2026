#ifndef VM_H
#define VM_H

#include "pt_regs.h"

#define PAGE_OFFSET   0xffffffc000000000UL
#ifndef PAGE_SIZE
#define PAGE_SIZE     (1UL << 12)
#endif
#define PMD_SIZE      (1UL << 21)
#define PGD_SIZE      (1UL << 30)

#define PA_TO_VA(pa) ((unsigned long)(pa) + PAGE_OFFSET)
#define VA_TO_PA(va) ((unsigned long)(va) - PAGE_OFFSET)

/* PTE descriptor bits (Sv39) */
#define PTE_V  (1UL << 0)  
#define PTE_R  (1UL << 1)
#define PTE_W  (1UL << 2)
#define PTE_X  (1UL << 3)
#define PTE_U  (1UL << 4)
#define PTE_G  (1UL << 5)
#define PTE_A  (1UL << 6)
#define PTE_D  (1UL << 7)

#define PROT_KERNEL (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PROT_MMIO (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)

#define MAKE_PTE(pa, flags) ((((unsigned long)(pa)) >> 12) << 10 | (flags))
#define SATP_SV39           (8UL << 60)
#define MAKE_SATP(pgd_pa)   (SATP_SV39 | ((unsigned long)(pgd_pa) >> 12))

#define USER_CODE_VA  0x0UL
#define USER_STACK_PAGES 16UL
#define USER_STACK_VA 0x0000003ffffff000UL
#define USER_TRAMPOLINE_VA  (USER_STACK_VA - USER_STACK_PAGES * PAGE_SIZE)
#define USER_SIGNAL_STACK_VA (USER_TRAMPOLINE_VA - PAGE_SIZE)

extern unsigned long pgd[512];

/* User space prot flags */
#define PROT_USER_RX  (PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)
#define PROT_USER_RW  (PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)

void setup_vm(void);
void drop_identity_map(void);
unsigned long *walk_user_pte(unsigned long *pgd_va, unsigned long va);
int map_one_page(unsigned long *pgd_va, unsigned long va,
                 unsigned long pa, unsigned long prot);
void map_pages(unsigned long *proc_pgd, unsigned long va, unsigned long size,
               unsigned long pa, unsigned long prot);
void unmap_user_pages(unsigned long *pgd_va, unsigned long va,
                      unsigned long size, int free_frames);
unsigned long *alloc_user_pgd(void);
void          free_user_pgd(unsigned long *pgd_va);
int           copy_user_pages(unsigned long *src_pgd, unsigned long *dst_pgd);
unsigned long lookup_user_pa(unsigned long *pgd_va, unsigned long va);
int           copy_from_user_pgd(unsigned long *pgd_va, void *dst,
                                 const void *src_user, unsigned long len);
int           copy_to_user_pgd(unsigned long *pgd_va, void *dst_user,
                               const void *src, unsigned long len);
int           copy_string_from_user_pgd(unsigned long *pgd_va, char *dst,
                                        const char *src_user,
                                        unsigned long max_len);
int handle_user_page_fault(struct pt_regs *regs);

#endif
