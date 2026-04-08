#include "mem_allocator.h"
#include "list.h"
#include "string.h"
#include "uart.h"

/* page frame 變成 chunk pool 後，不管這些 chunk 有無被用到，pool 都會留著*/

static struct buddy_allocator buddy_allocator;
static struct frame frame_array[BUDDY_MAX_PAGES];
struct chunk
{
    struct chunk *next;
};

struct chunk_pool
{
    unsigned int chunk_size; // chunk pool 裡面每一個 chunk 的大小
    struct chunk *free_list; // 指向該 pool 目前可用 chunk 的 linked list 開頭
};

struct page_pool_meta
{
    int pool_index;
};

#define SMALL_POOL_COUNT 13

static struct chunk_pool chunk_pools[SMALL_POOL_COUNT] = {
    {16, NULL},
    {32, NULL},
    {48, NULL},
    {64, NULL},
    {96, NULL},
    {128, NULL},
    {192, NULL},
    {256, NULL},
    {384, NULL},
    {512, NULL},
    {768, NULL},
    {1024, NULL},
    {2048, NULL},
};
static struct page_pool_meta pool_page_meta[BUDDY_MAX_PAGES];
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

/* 將任意位址對齊回所屬 page 的起始位址。 */
static unsigned long addr_to_page_base(unsigned long addr)
{
    return addr & ~(PAGE_SIZE - 1);
}

/* 根據 buddy XOR 規則，算出同 order 另一半 block 的 head index。 */
static unsigned long get_buddy_idx(unsigned long idx, unsigned int order)
{
    return idx ^ block_pages(order);
}

/* 輸出 block 的 page 範圍，方便 demo 時解釋 block 邊界。 */
static void log_block_range(unsigned long idx, unsigned int order)
{
    uart_puts("Range of pages: [");
    uart_dec(idx);
    uart_puts(", ");
    uart_dec(idx + block_pages(order) - 1);
    uart_puts("]");
}

static void log_free_area_add(unsigned long idx, unsigned int order)
{
    uart_puts("[+] Add");
    uart_puts(" page ");
    uart_dec(idx);
    uart_puts(" to order ");
    uart_dec(order);
    uart_puts(". ");
    log_block_range(idx, order);
    uart_puts("\n");
}

static void log_free_area_remove(unsigned long idx, unsigned int order)
{
    uart_puts("[-] Remove");
    uart_puts(" page ");
    uart_dec(idx);
    uart_puts(" from order ");
    uart_dec(order);
    uart_puts(". ");
    log_block_range(idx, order);
    uart_puts("\n");
}

static void log_next_page_addr(unsigned int order)
{
    struct frame *next_frame;

    if (list_empty(&buddy_allocator.free_area[order]))
    {
        uart_puts("none");
        return;
    }

    next_frame = list_first_entry(&buddy_allocator.free_area[order],
                                  struct frame,
                                  free_list);
    uart_hex(frame_idx_to_addr(&buddy_allocator,
                               (unsigned long)(next_frame - buddy_allocator.frames)));
}

static void log_page_alloc_event(unsigned long idx, unsigned int order)
{
    uart_puts("[Page] Allocate ");
    uart_hex(frame_idx_to_addr(&buddy_allocator, idx));
    uart_puts(" at order ");
    uart_dec(order);
    uart_puts(", page ");
    uart_dec(idx);
    uart_puts(". Next address at order ");
    uart_dec(order);
    uart_puts(": ");
    log_next_page_addr(order);
    uart_puts("\n");
}

static void log_page_free_event(unsigned long addr, unsigned long idx, unsigned int order)
{
    uart_puts("[Page] Free ");
    uart_hex(addr);
    uart_puts(" and add back to order ");
    uart_dec(order);
    uart_puts(", page ");
    uart_dec(idx);
    uart_puts(". Next address at order ");
    uart_dec(order);
    uart_puts(": ");
    log_next_page_addr(order);
    uart_puts("\n");
}

static void log_buddy_found(unsigned long idx, unsigned long buddy_idx, unsigned int order)
{
    uart_puts("[*] Buddy found! buddy idx: ");
    uart_dec(buddy_idx);
    uart_puts(" for page ");
    uart_dec(idx);
    uart_puts(" with order ");
    uart_dec(order);
    uart_puts("\n");
}

static void log_chunk_event(const char *action, void *ptr, unsigned int chunk_size)
{
    (void)action;
    (void)ptr;
    (void)chunk_size;
    /* 暫時關閉 chunk allocator 的 debug log，避免測試輸出過多。 */
}

static void log_pool_grow_event(unsigned long page_idx,
                                unsigned long page_addr,
                                unsigned int chunk_size,
                                unsigned int chunk_count)
{
    (void)page_idx;
    (void)page_addr;
    (void)chunk_size;
    (void)chunk_count;
    /* 暫時關閉 chunk pool grow log，之後需要追 allocator 行為再打開。 */
}

static void log_free_list_state(const char *action)
{
    unsigned int order;

    uart_puts("[Buddy] Free list blocks after ");
    uart_puts(action);
    uart_puts(": ");

    for (order = 0; order <= MAX_ORDER; order++)
    {
        if (order > 0)
        {
            uart_puts(", ");
        }

        uart_puts("order ");
        uart_dec(order);
        uart_puts(" = ");
        uart_dec(buddy_allocator.free_area_blocks[order]);
    }

    uart_puts("\n");
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
 * 只更新 block head 的 metadata。
 * advanced exercise 要求 allocate/free 維持 O(log n)，
 * 因此不能在每次 split / merge 時重寫整個 block 內所有 frame。
 */
static void set_block_head(struct buddy_allocator *buddy,
                           unsigned long idx,
                           unsigned int order,
                           int is_free)
{
    struct frame *frame = &buddy->frames[idx];

    frame->order = (int)order;
    frame->is_free = is_free;
    frame->is_head = 1;
    INIT_LIST_HEAD(&frame->free_list);
}

/* 清掉一個已經不再代表任何 block head 的 frame metadata。 */
static void clear_block_head(struct buddy_allocator *buddy, unsigned long idx)
{
    init_frame(&buddy->frames[idx]);
}

/*
 * 把一個 block 放進 free_area[order]。
 * 這裡會先把 metadata 標成 free block，再把 head 掛進 linked list。
 */
static void free_area_add(struct buddy_allocator *buddy, unsigned long idx, unsigned int order)
{
    struct frame *frame = &buddy->frames[idx];

    set_block_head(buddy, idx, order, 1);
    list_add_tail(&frame->free_list, &buddy->free_area[order]);
    buddy->free_area_blocks[order]++;
    log_free_area_add(idx, order);
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
    buddy_allocator.free_area_blocks[order]--;
    log_free_area_remove(idx, order);
}

/* 第一次使用 allocator 時才做全域初始化，避免開機流程太早碰 allocator。 */
static void buddy_lazy_init(void)
{
    unsigned long idx;

    if (buddy_ready)
    {
        return;
    }

    buddy_init(&buddy_allocator,
               BUDDY_MEM_BASE_ADDR,
               BUDDY_MEM_SIZE,
               frame_array);

    for (idx = 0; idx < BUDDY_MAX_PAGES; idx++)
    {
        pool_page_meta[idx].pool_index = -1;
    }

    buddy_ready = 1;
}

/* 找到可容納 size 的最小 chunk pool；若找不到則回傳 -1。 */
static int size_to_pool_index(unsigned long size)
{
    int i;

    for (i = 0; i < SMALL_POOL_COUNT; i++)
    {
        if (size <= chunk_pools[i].chunk_size)
        {
            return i;
        }
    }

    return -1;
}

/* 將一個新 page 切成固定大小 chunks，掛到指定 pool 的 free list。 */
static int pool_grow(int pool_index)
{
    unsigned long page_addr;
    unsigned long page_idx;
    unsigned int chunk_size;
    unsigned int chunk_count;
    unsigned int i;
    struct chunk *chunk;

    page_addr = (unsigned long)buddy_alloc(0);
    if (page_addr == 0)
    {
        return -1;
    }

    page_idx = addr_to_frame_idx(&buddy_allocator, page_addr);
    chunk_size = chunk_pools[pool_index].chunk_size;
    chunk_count = PAGE_SIZE / chunk_size;

    pool_page_meta[page_idx].pool_index = pool_index;

    for (i = chunk_count; i > 0; i--)
    {
        chunk = (struct chunk *)(page_addr + (unsigned long)(i - 1) * chunk_size);
        chunk->next = chunk_pools[pool_index].free_list;
        chunk_pools[pool_index].free_list = chunk;
    }

    log_pool_grow_event(page_idx, page_addr, chunk_size, chunk_count);

    return 0;
}

/* 小於一頁的配置走 chunk pool；pool 缺頁時向 buddy allocator 要新 page。 */
static void *pool_allocate(unsigned long size)
{
    int pool_index;
    struct chunk *chunk;

    pool_index = size_to_pool_index(size);
    if (pool_index < 0)
    {
        return NULL;
    }

    if (chunk_pools[pool_index].free_list == NULL)
    {
        if (pool_grow(pool_index) < 0)
        {
            return NULL;
        }
    }

    chunk = chunk_pools[pool_index].free_list;
    chunk_pools[pool_index].free_list = chunk->next;
    log_chunk_event("Allocate", (void *)chunk, chunk_pools[pool_index].chunk_size);

    return (void *)chunk;
}

/* 依 chunk 所在 page 的 metadata 找到 pool，並把 chunk 放回 free list。 */
static int pool_free_chunk(void *ptr)
{
    unsigned long addr;
    unsigned long page_base;
    unsigned long page_idx;
    unsigned long offset;
    int pool_index;
    unsigned int chunk_size;
    struct chunk *chunk;

    addr = (unsigned long)ptr;
    page_base = addr_to_page_base(addr);

    if (page_base < buddy_allocator.base_addr ||
        page_base >= frame_idx_to_addr(&buddy_allocator, buddy_allocator.page_count))
    {
        return -1;
    }

    page_idx = addr_to_frame_idx(&buddy_allocator, page_base);
    pool_index = pool_page_meta[page_idx].pool_index;
    if (pool_index < 0)
    {
        return -1;
    }

    chunk_size = chunk_pools[pool_index].chunk_size;
    offset = addr - page_base;
    if (offset % chunk_size != 0)
    {
        uart_puts("[Mem] Free ignored: pointer is not a chunk base\n");
        return 0;
    }

    chunk = (struct chunk *)ptr;
    chunk->next = chunk_pools[pool_index].free_list;
    chunk_pools[pool_index].free_list = chunk;
    log_chunk_event("Free", ptr, chunk_size);

    return 0;
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
        allocator->free_area_blocks[i] = 0;
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
        set_block_head(&buddy_allocator, idx, cur_order, 0);
        free_area_add(&buddy_allocator, buddy_idx, cur_order);
    }

    /*
     * 即使剛好 exact-fit，也要把整個 block 的 metadata 改成 allocated，
     * 只需要更新 block head 即可，member frame 不做 block-wide 重寫。
     */
    set_block_head(&buddy_allocator, idx, order, 0);
    log_page_alloc_event(idx, order);
    log_free_list_state("allocate");

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
    unsigned long old_idx;
    unsigned long retired_idx;
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
    set_block_head(&buddy_allocator, idx, cur_order, 1);

    while(cur_order < MAX_ORDER){
        struct frame *buddy_frame;

        buddy_idx = get_buddy_idx(idx, cur_order);
        if(buddy_idx >= buddy_allocator.page_count) break;

        buddy_frame = &buddy_allocator.frames[buddy_idx];
        if(!buddy_frame->is_free || !buddy_frame->is_head || buddy_frame->order != (int)cur_order)
            break;

        log_buddy_found(idx, buddy_idx, cur_order);

        /*
         * 只有當 buddy 也是同 order 的 free block head 時才能合併。
         * 合併後的新 head 一定是兩者中較小的 frame index。
         */
        free_area_remove(buddy_frame, buddy_idx, cur_order);
        old_idx = idx;
        retired_idx = old_idx;

        if (buddy_idx < idx)
        {
            idx = buddy_idx;
        }
        else
        {
            retired_idx = buddy_idx;
        }

        clear_block_head(&buddy_allocator, retired_idx);

        cur_order++;
        set_block_head(&buddy_allocator, idx, cur_order, 1);
    }

    free_area_add(&buddy_allocator, idx, cur_order);
    log_page_free_event(addr, idx, cur_order);
    log_free_list_state("free");
}

/* 對外的 byte-based 配置介面，內部轉成 buddy order 後交給 buddy_alloc。 */
void *allocate(unsigned long size)
{
    if (size == 0)
    {
        return NULL;
    }

    buddy_lazy_init();

    if (size <= chunk_pools[SMALL_POOL_COUNT - 1].chunk_size)
    {
        void *chunk_ptr = pool_allocate(size);

        if (chunk_ptr != NULL)
        {
            return chunk_ptr;
        }
    }

    return buddy_alloc(pages_to_order(size_to_page_count(size)));
}

/* 對外的釋放介面：pool chunk 回 pool，整頁配置則回 buddy allocator。 */
void free(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

    buddy_lazy_init();

    if (pool_free_chunk(ptr) == 0)
    {
        return;
    }

    buddy_free(ptr);
}
