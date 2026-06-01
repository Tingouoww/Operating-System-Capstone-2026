#include "mmap.h"
#include "task.h"

#define PAGE_ALIGN_DOWN(addr) \
    ((addr) & ~(PAGE_SIZE - 1))

static unsigned long page_align_up(unsigned long x) {
    return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int is_page_aligned(unsigned long x) {
    return (x & (PAGE_SIZE - 1)) == 0;
}

static int check_overlap_vma(struct task_struct *t,
                             unsigned long start,
                             unsigned long end) {
    for (int i = 0; i < MAX_MMAP_AREAS; i++) {
        struct vma *v = &t->vmas[i];

        if (!v->used)
            continue;
        if (start < v->end && v->start < end)
            return 0;
    }
    return 1;
}

static int range_has_user_mapping(struct task_struct *t,
                                  unsigned long start,
                                  unsigned long length) {
    for (unsigned long off = 0; off < length; off += PAGE_SIZE) {
        if (lookup_user_pa(t->pgd, start + off))
            return 1;
    }
    return 0;
}

static int range_is_available(struct task_struct *t,
                              unsigned long start,
                              unsigned long length) {
    unsigned long end = start + length;

    if (start == 0 || end > USER_MMAP_LIMIT || end < start)
        return 0;
    if (!check_overlap_vma(t, start, end))
        return 0;
    if (range_has_user_mapping(t, start, length))
        return 0;
    return 1;
}

static unsigned long find_free_mmap_area(struct task_struct *t,
                                         unsigned long length) {
    unsigned long base = USER_MMAP_BASE;

    while (base + length <= USER_MMAP_LIMIT && base + length >= base) {
        if (range_is_available(t, base, length))
            return base;
        base += PAGE_SIZE;
    }

    return 0;
}

static struct vma *alloc_vma(struct task_struct *t) {
    for (int i = 0; i < MAX_MMAP_AREAS; i++) {
        struct vma *v = &t->vmas[i];

        if (!v->used) {
            memset(v, 0, sizeof(*v));
            return v;
        }
    }

    return NULL;
}

static struct vma *create_vma(struct task_struct *t,
                              unsigned long start,
                              unsigned long end,
                              int prot,
                              int flags,
                              int type,
                              unsigned long src,
                              unsigned long src_len) {
    struct vma *v = alloc_vma(t);

    if (!v)
        return NULL;

    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->used = 1;
    v->type = type;
    v->src = src;
    v->src_len = src_len;
    return v;
}

long sys_mmap(unsigned long addr, unsigned long length, int prot, int flags) {
    struct task_struct *cur = get_current();
    unsigned long start;
    struct vma *v;

    if (!cur->pgd || length == 0)
        return 0;
    if (!(flags & MAP_ANONYMOUS))
        return 0;

    length = page_align_up(length);
    if (length == 0)
        return 0;

    if (addr != 0 && is_page_aligned(addr) &&
        range_is_available(cur, addr, length)) {
        start = addr;
    } else {
        start = find_free_mmap_area(cur, length);
        if (start == 0)
            return 0;
    }

    v = create_vma(cur, start, start + length, prot, flags,
                   VMA_ANONYMOUS, 0, 0);
    if (!v)
        return 0;

    if (flags & MAP_POPULATE) {
        for (unsigned long va = start; va < start + length; va += PAGE_SIZE) {
            if (populate_vma_page(cur, v, va) < 0) {
                unmap_user_pages(cur->pgd, start, length, 1);
                memset(v, 0, sizeof(*v));
                return 0;
            }
        }
    }

    return (long)start;
}

/* 確認 vma 是否為合法的使用者記憶體位址 */
struct vma *find_vma(struct task_struct *t, unsigned long addr){
    if(!t) return NULL;

    for(int i = 0; i < MAX_MMAP_AREAS; i++){
        struct vma *v = &t->vmas[i];

        if(!v->used) continue;

        if(v->start <= addr && addr < v->end) return v;
    }

    return NULL;
}

unsigned long vma_pte_flags(const struct vma *v){
    unsigned long flags;

    if (!v)
        return 0;

    flags = PTE_V | PTE_U | PTE_A | PTE_D;

    if (v->prot & PROT_READ)
        flags |= PTE_R;
    if (v->prot & PROT_WRITE) {
        flags |= PTE_R;
        flags |= PTE_W;
    }
    if (v->prot & PROT_EXEC)
        flags |= PTE_X;

    return flags;
}

int setup_user_exec_vmas(struct task_struct *t,
                         unsigned long code_src,
                         unsigned long code_size) {
    unsigned long code_end;
    unsigned long stack_start;

    if (!t || code_size == 0)
        return -1;

    memset(t->vmas, 0, sizeof(t->vmas));

    code_end = USER_CODE_VA + page_align_up(code_size);
    stack_start = USER_STACK_VA - (USER_STACK_PAGES - 1) * PAGE_SIZE;
    if (!create_vma(t, USER_CODE_VA, code_end,
                    PROT_READ | PROT_EXEC, 0,
                    VMA_TEXT, code_src, code_size))
        return -1;
    if (!create_vma(t, stack_start, USER_STACK_VA + PAGE_SIZE,
                    PROT_READ | PROT_WRITE, MAP_ANONYMOUS,
                    VMA_STACK, 0, 0))
        return -1;
    if (!create_vma(t, USER_SIGNAL_STACK_VA,
                    USER_SIGNAL_STACK_VA + PAGE_SIZE,
                    PROT_READ | PROT_WRITE, MAP_ANONYMOUS,
                    VMA_SIGNAL_STACK, 0, 0))
        return -1;
    if (!create_vma(t, USER_TRAMPOLINE_VA,
                    USER_TRAMPOLINE_VA + PAGE_SIZE,
                    PROT_READ | PROT_EXEC, 0,
                    VMA_TRAMPOLINE, 0, 0))
        return -1;

    return 0;
}

int populate_vma_page(struct task_struct *t, struct vma *v,
                      unsigned long fault_addr) {
    unsigned long page_va;
    unsigned long page;
    unsigned long flags;
    unsigned long copy_bytes;
    unsigned long file_off;

    if (!t || !t->pgd || !v)
        return -1;
    if (!v->used || fault_addr < v->start || fault_addr >= v->end)
        return -1;

    page_va = PAGE_ALIGN_DOWN(fault_addr);
    if (lookup_user_pa(t->pgd, page_va))
        return 0;

    flags = vma_pte_flags(v);
    if (!(flags & (PTE_R | PTE_W | PTE_X)))
        return -1;

    page = (unsigned long)buddy_alloc(0);
    if (!page)
        return -1;

    memset((void *)page, 0, PAGE_SIZE);

    switch (v->type) {
        case VMA_ANONYMOUS:
        case VMA_STACK:
        case VMA_SIGNAL_STACK:
            break;
        case VMA_TEXT:
            file_off = page_va - v->start;
            if (file_off < v->src_len) {
                copy_bytes = v->src_len - file_off;
                if (copy_bytes > PAGE_SIZE)
                    copy_bytes = PAGE_SIZE;
                mem_cpy((void *)page, (void *)(v->src + file_off), copy_bytes);
            }
            break;
        case VMA_TRAMPOLINE: {
            unsigned int *trampoline_code = (unsigned int *)page;

            trampoline_code[0] = 0x00b00893u;
            trampoline_code[1] = 0x00000073u;
            break;
        }
        default:
            buddy_free((void *)page);
            return -1;
    }

    if (map_one_page(t->pgd, page_va, VA_TO_PA(page), flags) < 0) {
        buddy_free((void *)page);
        return -1;
    }

    asm volatile("sfence.vma zero, zero" ::: "memory");
    return 0;
}
