#include "mem_allocator.h"
#include "list.h"
#include "string.h"
#include "uart.h"

static struct buddy_allocator buddy_allocator;
static struct frame frame_array[BUDDY_MAX_PAGES];
static int buddy_ready;

/* 回傳某個 order 的 block 會包含幾個 page。 */
static unsigned long block_pages(unsigned int order)
{
    return 1UL << order;
}

/* 把任意 byte 數向上換算成需要多少個 page。 */
static unsigned long size_to_page_count(unsigned long size)
{   
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* 找出可容納 pages 的最小 buddy order。 */
static unsigned int pages_to_order(unsigned long pages)
{
    unsigned int order = 0;
    unsigned long block = 1;

    while (block < pages)
    {
        block <<= 1;
        order++;
    }

    return order;
}

/* 把 frame index 轉回實體記憶體位址。 */
static unsigned long frame_idx_to_addr(struct buddy_allocator *buddy, unsigned long idx)
{
    return buddy->base_addr + idx * PAGE_SIZE;
}

/* 把位址換算成對應的 frame index。 */
static unsigned long addr_to_frame_idx(struct buddy_allocator *buddy, unsigned long addr)
{
    return (addr - buddy->base_addr) / PAGE_SIZE;
}

/* 根據 buddy XOR 規則，算出同 order 另一半 block 的 head index。 */
static unsigned long get_buddy_idx(unsigned long idx, unsigned int order)
{
    return idx ^ block_pages(order);
}

/* 輸出 block 的 page 範圍與 order，方便追 allocator 的狀態變化。 */
static void log_range(const char *prefix, unsigned long idx, unsigned int order)
{
    unsigned long start = frame_idx_to_addr(&buddy_allocator, idx);
    unsigned long end = start + block_pages(order) * PAGE_SIZE;

    uart_puts(prefix);
    uart_puts(" page ");
    uart_dec(idx);
    uart_puts(" order ");
    uart_dec(order);
    uart_puts(" addr [");
    uart_hex(start);
    uart_puts(", ");
    uart_hex(end);
    uart_puts(")\n");
}

/* 把單一 frame metadata 清成未使用狀態。 */
static void init_frame(struct frame *frame)
{
    /* 把單一 frame metadata 重設成初始狀態 */
    frame->order = FRAME_ORDER_UNUSED;
    frame->is_free = 0;
    frame->is_head = 0;
    INIT_LIST_HEAD(&frame->free_list);
}

/*
 * 依照一整個 block 的視角重寫 metadata。
 * head frame 保留 order；其餘 member frame 一律標成非 head。
 */
static void mark_block(struct buddy_allocator *buddy,
                       unsigned long idx,
                       unsigned int order,
                       int is_free)
{
    unsigned long pages = block_pages(order);
    unsigned long i;

    buddy->frames[idx].order = (int)order;
    buddy->frames[idx].is_free = is_free;
    buddy->frames[idx].is_head = 1;
    INIT_LIST_HEAD(&buddy->frames[idx].free_list);

    for (i = 1; i < pages; i++)
    {
        struct frame *member = &buddy->frames[idx + i];
        member->order = FRAME_ORDER_UNUSED;
        member->is_free = is_free;
        member->is_head = 0;
        INIT_LIST_HEAD(&member->free_list);
    }
}

/*
 * 把一個 block 放進 free_area[order]。
 * 這裡會先把 metadata 標成 free block，再把 head 掛進 linked list。
 */
static void free_area_add(struct buddy_allocator *buddy, unsigned long idx, unsigned int order)
{
    struct frame *frame = &buddy->frames[idx];

    mark_block(buddy, idx, order, 1);
    list_add_tail(&frame->free_list, &buddy->free_area[order]);
    log_range("[+] Add", idx, order);
}

/*
 * 把一個 free block 從 free list 拿掉。
 * 這個 helper 只處理 linked list 與 head 的 free 狀態，不負責重建整個 block metadata。
 */
static void free_area_remove(struct frame *frame, unsigned long idx, unsigned int order)
{
    list_del(&frame->free_list);
    INIT_LIST_HEAD(&frame->free_list);
    frame->is_free = 0;
    log_range("[-] Remove", idx, order);
}

/* 第一次使用 allocator 時才做全域初始化，避免開機流程太早碰 allocator。 */
static void buddy_lazy_init(void)
{
    if (buddy_ready)
    {
        return;
    }

    buddy_init(&buddy_allocator,
               BUDDY_MEM_BASE_ADDR,
               BUDDY_MEM_SIZE,
               frame_array);
    buddy_ready = 1;
}

/*
 * 初始化整個 buddy allocator。
 * 這裡會把可管理記憶體切成多個對齊的最大 block，並分別掛進對應 free list。
 */
void buddy_init(struct buddy_allocator *allocator,
                unsigned long base_addr, // memory base_addr
                unsigned long size, // memory size
                struct frame *frame_array)
{
    unsigned long idx = 0;
    unsigned long remaining;
    unsigned int order;
    int i;

    allocator->base_addr = base_addr;
    allocator->page_count = size / PAGE_SIZE;
    allocator->frames = frame_array;

    for (i = 0; i <= MAX_ORDER; i++)
    {
        INIT_LIST_HEAD(&allocator->free_area[i]);
    }

    for (idx = 0; idx < allocator->page_count; idx++)
    {
        init_frame(&allocator->frames[idx]);
    }

    idx = 0;
    remaining = allocator->page_count;

    while (remaining > 0)
    {
        order = MAX_ORDER;

        while (order > 0 &&
               (block_pages(order) > remaining ||
                (idx & (block_pages(order) - 1)) != 0))
        {
            order--;
        }

        free_area_add(allocator, idx, order);
        idx += block_pages(order);
        remaining -= block_pages(order);
    }
}

/*
 * 配置一個指定 order 的 block。
 * 流程是：
 * 1. 找到第一個可用的大 block。
 * 2. 從 free list 拿掉它。
 * 3. 一路往下 split，保留左半邊、把右半邊放回 free list。
 * 4. 最後把目標 block 標成 allocated 並回傳起始位址。
 */
void *buddy_alloc(unsigned int order)
{
    unsigned long idx;
    unsigned int cur_order;
    struct frame *frame;

    buddy_lazy_init();

    if (order > MAX_ORDER) return NULL;

    for(cur_order = order; cur_order <= MAX_ORDER; cur_order++){
        if(!(list_empty(&buddy_allocator.free_area[cur_order]))) break;
    };

    if (cur_order > MAX_ORDER) return NULL;

    frame = list_first_entry(&buddy_allocator.free_area[cur_order],
                                            struct frame,
                                            free_list);
    idx = (unsigned long)(frame - buddy_allocator.frames);
    free_area_remove(frame, idx, cur_order);

    while(cur_order > order){
        unsigned long buddy_idx;

        cur_order--;

        /*
         * 拆 block 時固定保留左半邊給 allocation path，
         * 右半邊作為新的 free buddy 掛回對應 order 的 free list。
         */
        buddy_idx = idx + block_pages(cur_order);
        mark_block(&buddy_allocator, idx, cur_order, 0);
        free_area_add(&buddy_allocator, buddy_idx, cur_order);
    }

    /*
     * 即使剛好 exact-fit，也要把整個 block 的 metadata 改成 allocated，
     * 避免 block 內 member frame 殘留先前 free block 的狀態。
     */
    mark_block(&buddy_allocator, idx, order, 0);

    // uart_puts("[Page] Allocate ");
    // uart_hex(frame_idx_to_addr(&buddy_allocator, idx));
    // uart_puts(" at order ");
    // uart_dec(order);
    // uart_puts(", page ");
    // uart_dec(idx);
    // uart_putc('.');
    // uart_puts(" Next address at order ");
    // uart_dec(order);
    // uart_puts(": ");
    
    // uart_puts("\n");

    return (void *)(buddy_allocator.base_addr + idx * PAGE_SIZE);
}

/*
 * 釋放一個先前由 buddy_alloc 回傳的 block。
 * 流程是：
 * 1. 驗證指標是否合法，且必須剛好指向某個 allocated block 的 head。
 * 2. 先把該 block 標成 free。
 * 3. 如果同 order 的 buddy 也是 free block，就持續合併。
 * 4. 最後把合併完成後的 block 掛回 free list。
 */
void buddy_free(void *ptr)
{
    unsigned long addr = (unsigned long)ptr;
    unsigned long idx;
    unsigned long buddy_idx;
    unsigned int cur_order;

    struct frame *frame;

    if (ptr == NULL) return;

    buddy_lazy_init();

    if (addr < buddy_allocator.base_addr ||
        addr >= frame_idx_to_addr(&buddy_allocator, buddy_allocator.page_count) ||
        (addr - buddy_allocator.base_addr) % PAGE_SIZE != 0)
    {
        uart_puts("[Page] Free ignored: invalid pointer\n");
        return;
    }

    idx = addr_to_frame_idx(&buddy_allocator, addr);
    frame = &buddy_allocator.frames[idx];

    if (!frame->is_head || frame->is_free || frame->order < 0)
    {
        uart_puts("[Page] Free ignored: pointer is not an allocated block head\n");
        return;
    }

    cur_order = (unsigned int)frame->order;
    mark_block(&buddy_allocator, idx, cur_order, 1);

    while(cur_order < MAX_ORDER){
        struct frame *buddy_frame;

        buddy_idx = get_buddy_idx(idx, cur_order);
        if(buddy_idx >= buddy_allocator.page_count) break;

        buddy_frame = &buddy_allocator.frames[buddy_idx];
        if(!buddy_frame->is_free || !buddy_frame->is_head || buddy_frame->order != (int)cur_order)
            break;

        /*
         * 只有當 buddy 也是同 order 的 free block head 時才能合併。
         * 合併後的新 head 一定是兩者中較小的 frame index。
         */
        free_area_remove(buddy_frame, buddy_idx, cur_order);

        if(buddy_idx < idx) idx = buddy_idx;

        cur_order++;
        frame = &buddy_allocator.frames[idx];
        mark_block(&buddy_allocator, idx, cur_order, 1);
    }

    free_area_add(&buddy_allocator, idx, cur_order);
}

/* 對外的 byte-based 配置介面，內部轉成 buddy order 後交給 buddy_alloc。 */
void *allocate(unsigned long size)
{
    unsigned long pages;
    unsigned int order;

    if (size == 0)
    {
        return NULL;
    }

    pages = size_to_page_count(size);
    order = pages_to_order(pages);

    return buddy_alloc(order);
}

/* 對外的釋放介面，直接轉呼叫 buddy_free。 */
void free(void *ptr)
{
    buddy_free(ptr);
}
