#ifndef MEM_ALLOCATOR_H
#define MEM_ALLOCATOR_H

#include "list.h"

#define PAGE_SIZE 4096UL
#define MAX_ORDER 10

#define BUDDY_MEM_BASE_ADDR 0x10000000UL
#define BUDDY_MEM_SIZE 0x10000000UL
#define BUDDY_MAX_PAGES (BUDDY_MEM_SIZE / PAGE_SIZE)
#define FRAME_ORDER_UNUSED (-1)

/* 定義 page frame 的結構 */
struct frame
{
    int order; // 只有在 frame 為 block head 時有意義
    int is_free; // 是否屬於 free block 
    int is_head; // 是否為 block head
    struct list_head free_list; // 只有 free block head 會掛入 free list
};

/* 定義此 buddy system 的狀態 */
struct buddy_allocator
{
    unsigned long base_addr;
    unsigned long page_count;
    struct frame *frames; // frame array pointer
    struct list_head free_area[MAX_ORDER + 1];
};

void buddy_init(struct buddy_allocator *buddy,
                unsigned long base_addr,
                unsigned long size,
                struct frame *frame_array);
void *buddy_alloc(unsigned int order);
void buddy_free(void *ptr);

void *allocate(unsigned long size);
void free(void *ptr);

#endif
