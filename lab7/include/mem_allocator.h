#ifndef MEM_ALLOCATOR_H
#define MEM_ALLOCATOR_H

#include "list.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif
#define MAX_ORDER 10
#define BUDDY_MAX_MANAGED_SIZE 0x80000000UL
#define BUDDY_MAX_PAGES (BUDDY_MAX_MANAGED_SIZE / PAGE_SIZE)
#define FRAME_ORDER_UNUSED           (-1)  /* initial / reset state                   */
/* 定義 page frame 的結構 */
struct frame
{
    int order; /* head: actual order; non-head: FRAME_ORDER_{FREE,ALLOCATED}_MEMBER */
    int is_free; // 是否屬於 free block 
    int is_head; // 是否為 block head
    struct list_head free_list; // 只有 free block head 會掛入 free list(自己就是可掛入 list 的節點)
};

/* 定義此 buddy system 的狀態 */
struct buddy_allocator
{
    unsigned long base_addr;
    unsigned long page_count;
    struct frame *frames; // 以 page frame index 直接 O(1) lookup metadata
    struct list_head free_area[MAX_ORDER + 1];
    unsigned int free_area_blocks[MAX_ORDER + 1]; // 每個 order 的 free block 數量，避免 log 時線性掃描
};

void buddy_init(struct buddy_allocator *buddy,
                unsigned long base_addr,
                unsigned long size,
                struct frame *frame_array);
void *buddy_alloc(unsigned int order);
void buddy_free(void *ptr);
void mem_allocator_init(const void *fdt);
void memory_reserve(unsigned long start, unsigned long size);

void *allocate(unsigned long size);
void free(void *ptr);

void page_ref_inc(unsigned long pa);
int page_ref_dec(unsigned long pa);
unsigned int page_ref_get(unsigned long pa);
void page_ref_set(unsigned long pa, unsigned int value);

#endif
