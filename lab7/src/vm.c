#include "vm.h"
#include "mem_allocator.h"
#include "utils.h"
#include "mmap.h"
#include "uart.h"

#ifdef QEMU
// QEMU DRAM 從 0x80000000 開始
#define PHY_RAM_BASE 0x80000000UL
#define DRAM_MAP_SIZE 0x40000000UL
#define QEMU_UART_BASE 0x10000000UL
#define QEMU_UART_SIZE PAGE_SIZE
#define QEMU_PLIC_BASE 0x0c000000UL
#define QEMU_PLIC_SIZE (4UL * 1024 * 1024)
#define QEMU_FW_CFG_BASE 0x10100000UL
#define QEMU_FW_CFG_SIZE PAGE_SIZE
#else
#define PHY_RAM_BASE  0x00000000UL
#define DRAM_MAP_SIZE 0x80000000UL
#define BOARD_FB_BASE   0x7f700000UL
#define BOARD_FB_SIZE   (8UL * 1024 * 1024)
#define BOARD_UART_BASE 0xd4017000UL
#define BOARD_UART_SIZE PAGE_SIZE
#define BOARD_PLIC_BASE 0xe0000000UL
#define BOARD_PLIC_SIZE (4UL * 1024 * 1024)
#endif

// PGD index = VPN[2] = (va >> 30) & 0x1ff
#define IDENTITY_PGD_IDX  ((PHY_RAM_BASE >> 30) & 0x1ff)
#define KERNEL_PGD_IDX    (((PHY_RAM_BASE + PAGE_OFFSET) >> 30) & 0x1ff)
#define DRAM_PMD_COUNT    (DRAM_MAP_SIZE / PGD_SIZE)
#define KERNEL_PT_POOL_PAGES 16

#define PAGE_ALIGN_DOWN(addr) \
    ((addr) & ~(PAGE_SIZE - 1))

#define SCAUSE_STORE_PAGE_FAULT 15

unsigned long pgd[512] __attribute__((aligned(4096))); // 全域 root page table
static unsigned long dram_pmd[DRAM_PMD_COUNT][512]
    __attribute__((aligned(4096)));
static unsigned long kernel_pt_pool[KERNEL_PT_POOL_PAGES][512]
    __attribute__((aligned(4096)));
static unsigned int kernel_pt_pool_used;

/* 開機前期使用，因為此時記憶體還沒初始化 */
static unsigned long *kernel_alloc_pt_page(void) {
    unsigned long *table;

    if (kernel_pt_pool_used >= KERNEL_PT_POOL_PAGES)
        while (1)
            ;

    table = kernel_pt_pool[kernel_pt_pool_used++];
    memset(table, 0, PAGE_SIZE);
    return table;
}

static void kernel_pagewalk(unsigned long *root_pgd,
                            unsigned long va,
                            unsigned long pa,
                            unsigned long prot) {
    unsigned long vpn2 = (va >> 30) & 0x1ff; // pgd index
    unsigned long vpn1 = (va >> 21) & 0x1ff; // pmd index
    unsigned long vpn0 = (va >> 12) & 0x1ff; // pte index
    unsigned long *pmd_table; // pmd table address
    unsigned long *pte_table; // pte table address

    if (!(root_pgd[vpn2] & PTE_V)) {
        pmd_table = kernel_alloc_pt_page();
        root_pgd[vpn2] = MAKE_PTE((unsigned long)pmd_table, PTE_V);
    } else {
        pmd_table = (unsigned long *)((root_pgd[vpn2] >> 10) << 12);
    }

    if (!(pmd_table[vpn1] & PTE_V)) {
        pte_table = kernel_alloc_pt_page();
        pmd_table[vpn1] = MAKE_PTE((unsigned long)pte_table, PTE_V);
    } else {
        pte_table = (unsigned long *)((pmd_table[vpn1] >> 10) << 12);
    }

    pte_table[vpn0] = MAKE_PTE(pa, prot);
}

static void kernel_map_pages(unsigned long *root_pgd,
                             unsigned long va,
                             unsigned long size,
                             unsigned long pa,
                             unsigned long prot) {
    for (unsigned long offset = 0; offset < size; offset += PAGE_SIZE)
        kernel_pagewalk(root_pgd, va + offset, pa + offset, prot);
}

static void map_kernel_io_regions(void) {
#ifdef QEMU
    kernel_map_pages(pgd, PAGE_OFFSET + QEMU_UART_BASE,
                     QEMU_UART_SIZE, QEMU_UART_BASE, PROT_MMIO);
    kernel_map_pages(pgd, PAGE_OFFSET + QEMU_PLIC_BASE,
                     QEMU_PLIC_SIZE, QEMU_PLIC_BASE, PROT_MMIO);
    kernel_map_pages(pgd, PAGE_OFFSET + QEMU_FW_CFG_BASE,
                     QEMU_FW_CFG_SIZE, QEMU_FW_CFG_BASE, PROT_MMIO);
#else
    kernel_map_pages(pgd, PAGE_OFFSET + BOARD_UART_BASE,
                     BOARD_UART_SIZE, BOARD_UART_BASE, PROT_MMIO);
    kernel_map_pages(pgd, PAGE_OFFSET + BOARD_PLIC_BASE,
                     BOARD_PLIC_SIZE, BOARD_PLIC_BASE, PROT_MMIO);
#endif
}

void setup_vm(void) {
    kernel_pt_pool_used = 0;

    for (unsigned long g = 0; g < DRAM_PMD_COUNT; g++) {
        unsigned long base = PHY_RAM_BASE + g * PGD_SIZE;

        // 每個 PMD 表負責 1GiB DRAM，內含 512 個 2MiB superpage。
        for (int i = 0; i < 512; i++) {
            dram_pmd[g][i] = MAKE_PTE(base + (unsigned long)i * PMD_SIZE,
                                      PROT_KERNEL);
        }

        // Identity mapping（DRAM）：VA = PA
        pgd[IDENTITY_PGD_IDX + g] = MAKE_PTE((unsigned long)dram_pmd[g], PTE_V);

        // Higher-half mapping（DRAM）：VA = PA + PAGE_OFFSET
        pgd[KERNEL_PGD_IDX + g] = MAKE_PTE((unsigned long)dram_pmd[g], PTE_V);
    }

    // MMIO / framebuffer 用 4KB page table 做 finer-grained、非 executable 映射
    map_kernel_io_regions();

    // 開 MMU：寫 satp、flush TLB
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n" // 同步虛擬記憶體轉換狀態的指令
        :
        : "r"(MAKE_SATP((unsigned long)pgd))
        : "memory"
    );
}

void drop_identity_map(void) {
    for (int i = 0; i < 256; i++)
        pgd[i] = 0;
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

/* 建立 va -> pa mapping */
static void pagewalk(unsigned long *proc_pgd, unsigned long va, unsigned long pa, unsigned long prot){
    unsigned long vpn2 = (va >> 30) & 0x1ff;
    unsigned long vpn1 = (va >> 21) & 0x1ff;
    unsigned long vpn0 = (va >> 12) & 0x1ff;
    unsigned long *pmd_table;
    unsigned long *pte_table;

    if (!(proc_pgd[vpn2] & PTE_V)) {
        pmd_table = buddy_alloc(0); // VA
        if (!pmd_table) return;
        memset(pmd_table, 0, PAGE_SIZE);
        proc_pgd[vpn2] = MAKE_PTE(VA_TO_PA((unsigned long)pmd_table), PTE_V);
    } else {
        pmd_table = (unsigned long *)PA_TO_VA((proc_pgd[vpn2] >> 10) << 12);
    }

    if (!(pmd_table[vpn1] & PTE_V)) {
        pte_table = buddy_alloc(0);
        if (!pte_table) return;
        memset(pte_table, 0, PAGE_SIZE);
        pmd_table[vpn1] = MAKE_PTE(VA_TO_PA((unsigned long)pte_table), PTE_V);
    } else {
        pte_table = (unsigned long *)PA_TO_VA((pmd_table[vpn1] >> 10) << 12);
    }

    pte_table[vpn0] = MAKE_PTE(pa, prot);
}

/* 查詢某個 user VA 對應的 PTE 指標，不建立任何東西 */
unsigned long *walk_user_pte(unsigned long *pgd_va, unsigned long va) {
    unsigned long vpn2 = (va >> 30) & 0x1ff;
    unsigned long vpn1 = (va >> 21) & 0x1ff;
    unsigned long vpn0 = (va >> 12) & 0x1ff;
    unsigned long *pmd;
    unsigned long *pte;

    if (!pgd_va || !(pgd_va[vpn2] & PTE_V))
        return NULL;

    pmd = (unsigned long *)PA_TO_VA((pgd_va[vpn2] >> 10) << 12);
    if (!(pmd[vpn1] & PTE_V) || (pmd[vpn1] & (PTE_R | PTE_W | PTE_X)))
        return NULL;

    pte = (unsigned long *)PA_TO_VA((pmd[vpn1] >> 10) << 12);
    return &pte[vpn0]; // 回傳指向 PTE entry 的指標 (呼叫方可以直接 R/W 這個 entry)
}

// 建立 VA -> PA 的映射關係, 多一個確認 map 真的寫入的驗證
int map_one_page(unsigned long *pgd_va, unsigned long va,
                 unsigned long pa, unsigned long prot) {
    pagewalk(pgd_va, va, pa, prot);

    if (lookup_user_pa(pgd_va, va) != pa)
        return -1;

    return 0;
}

/* 建立 VA -> PA 的映射關係 */
void map_pages(unsigned long *proc_pgd, unsigned long va, unsigned long size, unsigned long pa, unsigned long prot){
    for (unsigned long i = 0; i < size; i += PAGE_SIZE) {
        pagewalk(proc_pgd, va + i, pa + i, prot);
    }
}

/* 取消 [va, va+size) 的 user page 映射，free_frames=1 時同步釋放 refcount 歸零的 page frame */
void unmap_user_pages(unsigned long *pgd_va, unsigned long va,
                      unsigned long size, int free_frames){
    for (unsigned long offset = 0; offset < size; offset += PAGE_SIZE) {
        unsigned long *pte = walk_user_pte(pgd_va, va + offset);
        unsigned long data_pa;

        if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U))
            continue;

        data_pa = (*pte >> 10) << 12;
        *pte = 0;
        if (free_frames && page_ref_dec(data_pa) == 0)
            buddy_free((void *)PA_TO_VA(data_pa));
    }
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

unsigned long *alloc_user_pgd(void){
    unsigned long *new_pgd = (unsigned long *)buddy_alloc(0);
    if (!new_pgd) return NULL;
    memset(new_pgd, 0, PAGE_SIZE);
    for (int i = 256; i < 512; i++)
        new_pgd[i] = pgd[i];  // 複製 kernel mapping（DRAM + MMIO 高半段）
    return new_pgd;           
}

/* 釋放整個 user page table（PGD/PMD/PTE）及 refcount 歸零的 data frame，process 結束時呼叫 */
void free_user_pgd(unsigned long *pgd_va){
    if (!pgd_va) return;
    for (int i = 0; i < 256; i++) {
        if (!(pgd_va[i] & PTE_V)) continue;
        unsigned long *pmd = (unsigned long *)PA_TO_VA((pgd_va[i] >> 10) << 12);
        for (int j = 0; j < 512; j++) {
            if (!(pmd[j] & PTE_V)) continue;
            if (pmd[j] & (PTE_R | PTE_W | PTE_X)) continue; // superpage，跳過
            unsigned long *pte = (unsigned long *)PA_TO_VA((pmd[j] >> 10) << 12);
            // 釋放每個 leaf 資料頁（code / stack 頁框）
            for (int k = 0; k < 512; k++) {
                if (!(pte[k] & PTE_V)) continue;
                unsigned long data_pa = (pte[k] >> 10) << 12;
                if (page_ref_dec(data_pa) == 0)
                    buddy_free((void *)PA_TO_VA(data_pa));
            }
            buddy_free(pte);  // 釋放 PT 頁本身
        }
        buddy_free(pmd);
    }
    buddy_free(pgd_va);
}

int copy_user_pages(unsigned long *src_pgd, unsigned long *dst_pgd){
    for (int i = 0; i < 256; i++) {
        if (!(src_pgd[i] & PTE_V)) continue;
        unsigned long *src_pmd = (unsigned long *)PA_TO_VA((src_pgd[i] >> 10) << 12);
        for (int j = 0; j < 512; j++) {
            if (!(src_pmd[j] & PTE_V)) continue;
            if (src_pmd[j] & (PTE_R | PTE_W | PTE_X)) continue; // superpage，跳過
            unsigned long *src_pte = (unsigned long *)PA_TO_VA((src_pmd[j] >> 10) << 12);
            for (int k = 0; k < 512; k++) {
                if (!(src_pte[k] & PTE_V)) continue;
                unsigned long src_pa = (src_pte[k] >> 10) << 12;
                unsigned long prot   = src_pte[k] & 0x3FF;
                unsigned long *new_frame = (unsigned long *)buddy_alloc(0);
                if (!new_frame) return -1;
                mem_cpy(new_frame, (void *)PA_TO_VA(src_pa), PAGE_SIZE);
                page_ref_set(VA_TO_PA((unsigned long)new_frame), 1);
                unsigned long va = ((unsigned long)i << 30) |
                                   ((unsigned long)j << 21) |
                                   ((unsigned long)k << 12);
                pagewalk(dst_pgd, va, VA_TO_PA((unsigned long)new_frame), prot);
            }
        }
    }
    return 0;
}

int fork_user_pages_cow(unsigned long *src_pgd, unsigned long *dst_pgd) {
    for (int i = 0; i < 256; i++) {
        unsigned long *src_pmd;

        if (!(src_pgd[i] & PTE_V))
            continue;

        src_pmd = (unsigned long *)PA_TO_VA((src_pgd[i] >> 10) << 12);
        for (int j = 0; j < 512; j++) {
            unsigned long *src_pte;

            if (!(src_pmd[j] & PTE_V))
                continue;
            if (src_pmd[j] & (PTE_R | PTE_W | PTE_X))
                continue; // superpage，跳過

            src_pte = (unsigned long *)PA_TO_VA((src_pmd[j] >> 10) << 12);
            for (int k = 0; k < 512; k++) {
                unsigned long src_entry = src_pte[k];
                unsigned long src_pa;
                unsigned long shared_flags;
                unsigned long va;

                if (!(src_entry & PTE_V) || !(src_entry & PTE_U))
                    continue;

                src_pa = (src_entry >> 10) << 12;
                shared_flags = src_entry & 0x3FF;
                if (shared_flags & PTE_W) {
                    shared_flags = (shared_flags & ~PTE_W) | PTE_COW;
                    src_pte[k] = MAKE_PTE(src_pa, shared_flags); // parent 也改成 read-only + CoW
                }

                va = ((unsigned long)i << 30) |
                     ((unsigned long)j << 21) |
                     ((unsigned long)k << 12);
                if (map_one_page(dst_pgd, va, src_pa, shared_flags) < 0)
                    return -1;

                /* 紀錄 physical page 的引用次數 */
                if (page_ref_get(src_pa) == 0)
                    page_ref_set(src_pa, 1);
                page_ref_inc(src_pa);
            }
        }
    }

    asm volatile("sfence.vma zero, zero" ::: "memory");
    return 0;
}

/* 把某個 user virtual address 查成 physical address */
unsigned long lookup_user_pa(unsigned long *pgd_va, unsigned long va){
    unsigned long vpn2 = (va >> 30) & 0x1ff;
    unsigned long vpn1 = (va >> 21) & 0x1ff;
    unsigned long vpn0 = (va >> 12) & 0x1ff;
    if (!(pgd_va[vpn2] & PTE_V)) return 0;
    unsigned long *pmd = (unsigned long *)PA_TO_VA((pgd_va[vpn2] >> 10) << 12);
    if (!(pmd[vpn1] & PTE_V)) return 0;
    unsigned long *pte = (unsigned long *)PA_TO_VA((pmd[vpn1] >> 10) << 12);
    if (!(pte[vpn0] & PTE_V)) return 0;
    /* 拒絕 kernel page：沒有 PTE_U 的 entry 不屬於 user space */
    if (!(pte[vpn0] & PTE_U)) return 0;
    return (pte[vpn0] >> 10) << 12;
}

/* 從 user space 複製一段原始資料到 kernel buffer */
int copy_from_user_pgd(unsigned long *pgd_va, void *dst,
                       const void *src_user, unsigned long len){
    unsigned char *dst_bytes = (unsigned char *)dst;
    unsigned long src_va = (unsigned long)src_user;
    struct task_struct *cur = get_current();

    if (!pgd_va) return -1;

    while (len > 0) {
        unsigned long src_pa = lookup_user_pa(pgd_va, src_va);
        unsigned long page_off;
        unsigned long chunk;

        if (!src_pa && cur && cur->pgd == pgd_va) {
            struct vma *v = find_vma(cur, src_va);

            if (v && populate_vma_page(cur, v, src_va) == 0)
                src_pa = lookup_user_pa(pgd_va, src_va);
        }
        if (!src_pa) return -1;

        page_off = src_va & (PAGE_SIZE - 1);
        chunk = PAGE_SIZE - page_off;
        if (chunk > len) chunk = len;

        mem_cpy(dst_bytes, (const void *)(PA_TO_VA(src_pa) + page_off), chunk);
        dst_bytes += chunk;
        src_va += chunk;
        len -= chunk;
    }
    return 0;
}

int copy_to_user_pgd(unsigned long *pgd_va, void *dst_user,
                     const void *src, unsigned long len){
    const unsigned char *src_bytes = (const unsigned char *)src;
    unsigned long dst_va = (unsigned long)dst_user;
    struct task_struct *cur = get_current();

    if (!pgd_va) return -1;

    while (len > 0) {
        unsigned long dst_pa = lookup_user_pa(pgd_va, dst_va);
        unsigned long *pte = walk_user_pte(pgd_va, dst_va);
        unsigned long page_off;
        unsigned long chunk;
        struct vma *v = NULL;

        if (!dst_pa && cur && cur->pgd == pgd_va) {
            v = find_vma(cur, dst_va);

            if (v && populate_vma_page(cur, v, dst_va) == 0)
                dst_pa = lookup_user_pa(pgd_va, dst_va);
        }
        if (!dst_pa) return -1;

        pte = walk_user_pte(pgd_va, dst_va);
        if (pte && (*pte & PTE_V) && !(*pte & PTE_W)) {
            if (!cur || cur->pgd != pgd_va)
                return -1;

            if (!v)
                v = find_vma(cur, dst_va);
            if (handle_cow_fault(cur, v, PAGE_ALIGN_DOWN(dst_va), pte) < 0)
                return -1;

            dst_pa = lookup_user_pa(pgd_va, dst_va);
            if (!dst_pa)
                return -1;
        }

        page_off = dst_va & (PAGE_SIZE - 1);
        chunk = PAGE_SIZE - page_off;
        if (chunk > len) chunk = len;

        mem_cpy((void *)(PA_TO_VA(dst_pa) + page_off), src_bytes, chunk);
        src_bytes += chunk;
        dst_va += chunk;
        len -= chunk;
    }
    return 0;
}

/* 把使用者程式提供的字串指標安全地複製到 kernel 的 buffer */
int copy_string_from_user_pgd(unsigned long *pgd_va, char *dst,
                              const char *src_user, unsigned long max_len){
    unsigned long i;

    if (!pgd_va || !dst || !src_user || max_len == 0)
        return -1;

    for (i = 0; i < max_len; i++) {
        // 從 src_user 開始，一次複製 1 個 byte 到 dst[i]
        if (copy_from_user_pgd(pgd_va, &dst[i], src_user + i, 1) < 0)
            return -1;
        if (dst[i] == '\0')
            return 0;
    }

    dst[max_len - 1] = '\0';
    return -1;
}

int handle_cow_fault(struct task_struct *cur, struct vma *v,
                     unsigned long page_va, unsigned long *pte) {
    unsigned long old_pa;
    unsigned long old_flags;
    unsigned long new_flags;
    unsigned long new_page;

    if (!cur || !v || !pte)
        return -1;
    if (!(v->prot & PROT_WRITE)) // prot 指原本記憶體的權限
        return -1;
    if (!(*pte & PTE_V) || !(*pte & PTE_U) || !(*pte & PTE_COW))
        return -1;

    old_pa = (*pte >> 10) << 12;
    old_flags = *pte & 0x3FF;
    new_flags = (old_flags | PTE_W) & ~PTE_COW; // 設回 W 然後移除 CoW

    // 分配新 page
    new_page = (unsigned long)buddy_alloc(0);
    if (!new_page)
        return -1;

    mem_cpy((void *)new_page, (const void *)PA_TO_VA(old_pa), PAGE_SIZE);
    page_ref_set(VA_TO_PA(new_page), 1);
    *pte = MAKE_PTE(VA_TO_PA(new_page), new_flags); // 把 pte 改指向新的 page

    if (page_ref_dec(old_pa) == 0)
        buddy_free((void *)PA_TO_VA(old_pa));

    asm volatile("sfence.vma %0, zero" :: "r"(page_va) : "memory");
    return 0;
}

int handle_user_page_fault(struct pt_regs *regs){
    
    struct task_struct *cur = get_current();
    
    if(!cur || !cur->pgd) return -1;

    unsigned long addr = regs->stval; // stval 存試圖存取的 fault address (va)
    unsigned long page_va = PAGE_ALIGN_DOWN(addr);
    struct vma *v = find_vma(cur, addr); // 確認是否為合法位址

    if(!v) return -1;

    unsigned long *pte = walk_user_pte(cur->pgd, page_va);
    if(!pte || !(*pte & PTE_V)){ // VA no mapping
        if(populate_vma_page(cur, v, addr) < 0) return -1;

        uart_puts("[Translation fault]: ");
        uart_hex(addr);
        uart_puts("\n");

        return 0;
    }

    // 若為寫入 -> 寫入時 copy (CoW)
    if (regs->scause == SCAUSE_STORE_PAGE_FAULT) { 
        if (handle_cow_fault(cur, v, page_va, pte) == 0) {
            uart_puts("[Permission fault]: ");
            uart_hex(addr);
            uart_puts("\n");
            return 0;
        }
    }

    return -1;
}
