#include "vm.h"
#include "mem_allocator.h"
#include "utils.h"

#ifdef QEMU
// QEMU DRAM 從 0x80000000 開始
#define PHY_RAM_BASE 0x80000000UL
// QEMU MMIO（PLIC=0x0c000000, FW_CFG=0x10100000）在第一個 GiB
#define PHY_MMIO_BASE 0x00000000UL
#else
// Board: RAM 和 MMIO 都在第一個 GiB（0x00000000-0x3FFFFFFF）
#define PHY_RAM_BASE  0x00000000UL
#endif

// PGD index = VPN[2] = (va >> 30) & 0x1ff
#define IDENTITY_PGD_IDX  ((PHY_RAM_BASE >> 30) & 0x1ff)
#define KERNEL_PGD_IDX    (((PHY_RAM_BASE + PAGE_OFFSET) >> 30) & 0x1ff)

unsigned long pgd[512] __attribute__((aligned(4096)));
static unsigned long pmd[512]     __attribute__((aligned(4096)));  // DRAM region

#ifdef QEMU
// QEMU 需要額外一張 PMD 來 map 低位址 MMIO（PLIC, FW_CFG 等）
static unsigned long pmd_mmio[512] __attribute__((aligned(4096)));
#define MMIO_IDENTITY_PGD_IDX  ((PHY_MMIO_BASE >> 30) & 0x1ff)          // 0
#define MMIO_KERNEL_PGD_IDX    (((PHY_MMIO_BASE + PAGE_OFFSET) >> 30) & 0x1ff) // 256
#endif

void setup_vm(void) {
    // 1. 填 DRAM PMD：512 個 2MiB superpage，共覆蓋 1GiB
    for (int i = 0; i < 512; i++) {
        pmd[i] = MAKE_PTE(PHY_RAM_BASE + (unsigned long)i * PMD_SIZE,
                          PROT_KERNEL);
    }

    // 2. Identity mapping（DRAM）：VA = PA
    pgd[IDENTITY_PGD_IDX] = MAKE_PTE((unsigned long)pmd, PTE_V);

    // 3. Higher-half mapping（DRAM）：VA = PA + PAGE_OFFSET
    pgd[KERNEL_PGD_IDX] = MAKE_PTE((unsigned long)pmd, PTE_V);

#ifdef QEMU
    // 4. QEMU 低位址 MMIO（PLIC=0x0c000000, FW_CFG=0x10100000）
    //    用 PROT_MMIO（無 X bit）map 第一個 GiB
    for (int i = 0; i < 512; i++) {
        pmd_mmio[i] = MAKE_PTE(PHY_MMIO_BASE + (unsigned long)i * PMD_SIZE,
                               PROT_MMIO);
    }
    pgd[MMIO_IDENTITY_PGD_IDX] = MAKE_PTE((unsigned long)pmd_mmio, PTE_V);
    pgd[MMIO_KERNEL_PGD_IDX]   = MAKE_PTE((unsigned long)pmd_mmio, PTE_V);
#endif

    // 5. 開 MMU：寫 satp、flush TLB
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(MAKE_SATP((unsigned long)pgd))
        : "memory"
    );
}

void drop_identity_map(void) {
    pgd[IDENTITY_PGD_IDX] = 0;   // 清掉 DRAM identity
#ifdef QEMU
    pgd[MMIO_IDENTITY_PGD_IDX] = 0;  // 清掉 MMIO identity（PGD[0]）
#endif
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

static void pagewalk(unsigned long *proc_pgd, unsigned long va, unsigned long pa, unsigned long prot){
    unsigned long vpn2 = (va >> 30) & 0x1ff;
    unsigned long vpn1 = (va >> 21) & 0x1ff;
    unsigned long vpn0 = (va >> 12) & 0x1ff;
    unsigned long *pmd_table;
    unsigned long *pte_table;

    if (!(proc_pgd[vpn2] & PTE_V)) {
        pmd_table = buddy_alloc(0);
        memset(pmd_table, 0, PAGE_SIZE);
        proc_pgd[vpn2] = MAKE_PTE(VA_TO_PA((unsigned long)pmd_table), PTE_V);
    } else {
        pmd_table = (unsigned long *)PA_TO_VA((proc_pgd[vpn2] >> 10) << 12);
    }

    if (!(pmd_table[vpn1] & PTE_V)) {
        pte_table = buddy_alloc(0);
        memset(pte_table, 0, PAGE_SIZE);
        pmd_table[vpn1] = MAKE_PTE(VA_TO_PA((unsigned long)pte_table), PTE_V);
    } else {
        pte_table = (unsigned long *)PA_TO_VA((pmd_table[vpn1] >> 10) << 12);
    }

    pte_table[vpn0] = MAKE_PTE(pa, prot);
}

void map_pages(unsigned long *proc_pgd, unsigned long va, unsigned long size, unsigned long pa, unsigned long prot){
    for (unsigned long i = 0; i < size; i += PAGE_SIZE) {
        pagewalk(proc_pgd, va + i, pa + i, prot);
    }
}