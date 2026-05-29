#include "mmap.h"

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

long sys_mmap(unsigned long addr, unsigned long length, int prot, int flags) {
    struct task_struct *cur = get_current();
    unsigned long start;
    unsigned long pte_flags = PTE_V | PTE_U | PTE_A | PTE_D;
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

    v = alloc_vma(cur);
    if (!v)
        return 0;

    if (prot & PROT_READ)
        pte_flags |= PTE_R;
    if (prot & PROT_WRITE)
        pte_flags |= PTE_W;
    if (prot & PROT_EXEC)
        pte_flags |= PTE_X;

    /* Ex1 expects anonymous mmap pages to be usable immediately. */
    if (pte_flags & (PTE_R | PTE_W | PTE_X)) {
        for (unsigned long off = 0; off < length; off += PAGE_SIZE) {
            unsigned long page = (unsigned long)buddy_alloc(0);

            if (!page) {
                unmap_user_pages(cur->pgd, start, off, 1);
                return 0;
            }

            memset((void *)page, 0, PAGE_SIZE);
            map_pages(cur->pgd, start + off, PAGE_SIZE, VA_TO_PA(page), pte_flags);
            if (lookup_user_pa(cur->pgd, start + off) != VA_TO_PA(page)) {
                buddy_free((void *)page);
                unmap_user_pages(cur->pgd, start, off, 1);
                return 0;
            }
        }
    }

    v->start = start;
    v->end = start + length;
    v->prot = prot;
    v->flags = flags;
    v->used = 1;

    return (long)start;
}
