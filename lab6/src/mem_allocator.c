#include "mem_allocator.h"
#include "fdt.h"
#include "list.h"
#include "utils.h"
#include "uart.h"
#include "vm.h"

/* page frame 變成 chunk pool 後，不管這些 chunk 有無被用到，pool 都會留著*/

static struct buddy_allocator buddy_allocator;
/* 每個 page 一個 bitset-like 標記，記錄該 page frame 是否被保留。 */
static unsigned char reserved_page_map[BUDDY_MAX_PAGES];
/* 保留一份 DTB 指標，讓 lazy init 仍能回頭完成初始化。 */
static const void *allocator_fdt;
static unsigned long startup_alloc_cursor;

/* ----- Prototype -----*/
// static void log_block_range(unsigned long idx, unsigned int order);
// static void log_free_area_add(unsigned long idx, unsigned int order);
// static void log_free_area_remove(unsigned long idx, unsigned int order);
// static void log_next_page_addr(unsigned int order);
// static void log_page_alloc_event(unsigned long idx, unsigned int order);
// static void log_page_free_event(unsigned long addr, unsigned long idx, unsigned int order);
// static void log_buddy_found(unsigned long idx, unsigned long buddy_idx, unsigned int order);
// static void log_memory_region(const char *label, unsigned long start, unsigned long size);
// static void log_reserve_event(unsigned long start,
//                               unsigned long end,
//                               unsigned long first_page,
//                               unsigned long last_page);
// static void log_chunk_event(const char *action, void *ptr, unsigned int chunk_size);
// static void log_pool_grow_event(unsigned long page_idx,
//                                 unsigned long page_addr,
//                                 unsigned int chunk_size,
//                                 unsigned int chunk_count);
// static void log_free_list_state(const char *action);
static void *startup_alloc(unsigned long size);

extern char __kernel_start;
extern char __kernel_end;

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
static struct page_pool_meta pool_page_meta[BUDDY_MAX_PAGES]; // 記錄第 page_idx 頁是拿來當哪個 pool 的 chunk page
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

/* 找出大於等於 addr 的最小 page 邊界*/
static unsigned long align_up_to_page(unsigned long addr)
{
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);// 先往上推再向下對齊
}

static unsigned long align_down_to_page(unsigned long addr)
{
    return addr & ~(PAGE_SIZE - 1);
}

/* 根據 buddy XOR 規則，算出同 order 另一半 block 的 head index。 */
static unsigned long get_buddy_idx(unsigned long idx, unsigned int order)
{
    return idx ^ block_pages(order); //  1UL << order
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
 * 只更新 head frame 的 metadata（O(1)），不標記 block 內其他 frames，
 * 以符合 Advanced Exercise 要求的 O(log n) allocate/free。
 */
static void free_area_add(struct buddy_allocator *buddy, unsigned long idx, unsigned int order)
{
    struct frame *frame = &buddy->frames[idx];

    set_block_head(buddy, idx, order, 1);
    list_add_tail(&frame->free_list, &buddy->free_area[order]);
    buddy->free_area_blocks[order]++;
    //log_free_area_add(idx, order);
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
    // log_free_area_remove(idx, order);
}

/* 初始化 chunk page 狀態 */
static void reset_dynamic_allocators(void)
{
    unsigned int i;
    unsigned long idx;

    /* 重新建 allocator layout， chunk pools 也要一起回到初始狀態 */
    for (i = 0; i < SMALL_POOL_COUNT; i++)
    {
        chunk_pools[i].free_list = NULL;
    }

    for (idx = 0; idx < BUDDY_MAX_PAGES; idx++)
    {
        pool_page_meta[idx].pool_index = -1;
    }
}

static void add_free_range(unsigned long start_idx, unsigned long page_count)
{
    unsigned long idx = start_idx;
    unsigned long remaining = page_count;

    /* 把一段連續可用 pages 切成多個對齊的 blocks 掛回 free list。 */
    while (remaining > 0)
    {
        unsigned int order = MAX_ORDER;

        while (order > 0 &&
               (block_pages(order) > remaining || // 太大
                (idx & (block_pages(order) - 1)) != 0)) // 對齊檢查(沒對齊)
        {
            order--;
        }

        free_area_add(&buddy_allocator, idx, order);
        idx += block_pages(order);
        remaining -= block_pages(order);
    }
}

static void *startup_alloc(unsigned long size)
{
    /* 替 frame_array 找一塊連續的實體記憶體，並立刻標記成 reserved，
        避免後面被 buddy allocator 當成可分配頁面回收 */
    
    unsigned long alloc_start;
    unsigned long pages;
    unsigned long idx;

    /* 早期啟動 allocator 至少要有有效需求，且必須已知 managed page 數。 */
    if (size == 0 || buddy_allocator.page_count == 0)
    {
        return NULL;
    }

    /* startup allocator 仍以 page 為最小單位配置。 */
    pages = size_to_page_count(size);
    if (pages == 0)
    {
        return NULL;
    }

    /* bump cursor 永遠從 page boundary 開始找下一段可用空間。 */
    alloc_start = align_up_to_page(startup_alloc_cursor);
    if (alloc_start < buddy_allocator.base_addr)
    {
        alloc_start = buddy_allocator.base_addr;
    }

    idx = addr_to_frame_idx(&buddy_allocator, alloc_start);
    while (idx + pages <= buddy_allocator.page_count)
    {
        unsigned long run_start = idx;

        /* 先跳過所有已被標記為 reserved 的 pages。 */
        while (idx < buddy_allocator.page_count && reserved_page_map[idx] != 0)
        {
            idx++;
        }

        run_start = idx;
        /* 收集接下來這段連續未保留的 run。 */
        while (idx < buddy_allocator.page_count && reserved_page_map[idx] == 0)
        {
            idx++;
        }

        if (idx - run_start >= pages)
        {
            unsigned long addr = frame_idx_to_addr(&buddy_allocator, run_start);
            unsigned long alloc_size = pages * PAGE_SIZE;

            /*
             * startup allocator 只在 early boot 使用；
             * 找到夠大的連續區後，前移 cursor，並立刻把該範圍標成 reserved，
             * 避免之後 frame array 被 buddy allocator 當成 free pages 回收。
             */
            startup_alloc_cursor = addr + alloc_size;
            memory_reserve(addr, size);
            return (void *)PA_TO_VA(addr);  // 對外回傳 VA
        }
    }

    /* 第一個 usable run 之後也找不到足夠大的連續空間。 */
    return NULL;
}

static void buddy_build_free_areas(void)
{
    int i;
    unsigned long idx;

    /*
     * 基於 reserved_page_map 重建 allocator 視圖：
     * 只有未保留的連續區段會被切成 free buddy blocks。
     */
    for (i = 0; i <= MAX_ORDER; i++)
    {
        INIT_LIST_HEAD(&buddy_allocator.free_area[i]);
        buddy_allocator.free_area_blocks[i] = 0; // 紀錄數量
    }

    for (idx = 0; idx < buddy_allocator.page_count; idx++)
    {
        init_frame(&buddy_allocator.frames[idx]);
    }

    idx = 0;
    while (idx < buddy_allocator.page_count)
    {
        unsigned long run_start;

        while (idx < buddy_allocator.page_count && reserved_page_map[idx] != 0)
        {
            idx++;
        }

        run_start = idx;
        while (idx < buddy_allocator.page_count && reserved_page_map[idx] == 0)
        {
            idx++;
        }

        if (idx > run_start)
        {
            add_free_range(run_start, idx - run_start);
        }
    }
}

void memory_reserve(unsigned long start, unsigned long size)
{
    unsigned long reserve_start;
    unsigned long reserve_end;
    unsigned long managed_start;
    unsigned long managed_end;
    unsigned long idx;
    unsigned long first_page;
    unsigned long last_page;

    if (size == 0 || buddy_allocator.page_count == 0)
    {
        return;
    }

    /* 以 page 為單位保留，避免 allocator 仍把同一頁的一部分分配出去。 */
    reserve_start = align_down_to_page(start); // 起點往下對齊到 page 開頭。
    if (start + size < start)
    {
        reserve_end = ~0UL;
    }
    else
    {
        reserve_end = align_up_to_page(start + size); // 終點往上對齊到 page 結尾
    }

    // buddy allocator 實際能管的區間 [managed_start, managed_end)
    managed_start = buddy_allocator.base_addr;
    managed_end = frame_idx_to_addr(&buddy_allocator, buddy_allocator.page_count);

    // 保留區間跟 allocator 管理範圍完全沒交集就直接返回
    if (reserve_end <= managed_start || reserve_start >= managed_end)
    {
        return;
    }

    // 保留範圍裁切到 allocator 管理範圍內
    if (reserve_start < managed_start)
    {
        reserve_start = managed_start;
    }
    if (reserve_end > managed_end)
    {
        reserve_end = managed_end;
    }

    first_page = addr_to_frame_idx(&buddy_allocator, reserve_start);
    last_page = addr_to_frame_idx(&buddy_allocator, reserve_end);
    //log_reserve_event(reserve_start, reserve_end, first_page, last_page);

    for (idx = first_page; idx < last_page; idx++)
    {
        reserved_page_map[idx] = 1;
    }
}

static void reserve_memory_regions_from_fdt(const void *fdt)
{
    struct fdt_memory_region reserved_regions[32];
    // __kernel_start / __kernel_end 是 VA（linker script 放在 higher-half）
    // memory_reserve 用 PA 做邊界計算，要先轉換
    unsigned long kernel_start = VA_TO_PA((unsigned long)&__kernel_start);
    unsigned long kernel_size = (unsigned long)(&__kernel_end - &__kernel_start);
    unsigned long initrd_start = 0;
    unsigned long initrd_end = 0;
    int region_count;
    int i;

    /* 四類保留來源：kernel image、dtb blob、initramfs、reserved-memory */
    //log_memory_region("kernel", kernel_start, kernel_size);
    memory_reserve(kernel_start, kernel_size);

    //log_memory_region("dtb", (unsigned long)fdt, fdt_totalsize(fdt));
    memory_reserve((unsigned long)fdt, fdt_totalsize(fdt));

    if (fdt_get_initrd_range(fdt, &initrd_start, &initrd_end) == 0 &&
        initrd_end > initrd_start)
    {
        //log_memory_region("initrd", initrd_start, initrd_end - initrd_start);
        memory_reserve(initrd_start, initrd_end - initrd_start);
    }
    else
    {
        //log_memory_region("initrd", 0, 0);
    }

    region_count = fdt_get_reserved_memory_regions(fdt,
                                                   reserved_regions,
                                                   (int)(sizeof(reserved_regions) /
                                                         sizeof(reserved_regions[0])));
    if (region_count <= 0)
    {
        return;
    }

    for (i = 0; i < region_count; i++)
    {
        memory_reserve(reserved_regions[i].start, reserved_regions[i].size);
    }
}

void mem_allocator_init(const void *fdt)
{
    unsigned long mem_base = 0; // /memory first memory region start address
    unsigned long mem_size = 0; // /memory first memory region size
    /* 把 mem_base 向上對齊到 page boundary 後的起點。
因為 buddy allocator 以 page 為單位管理，起點要 page-aligned。*/
    unsigned long aligned_base; 
    unsigned long aligned_end;
    unsigned long managed_size;
    unsigned long max_size = BUDDY_MAX_PAGES * PAGE_SIZE;
    unsigned long frame_array_size;
    void *frame_array;

    if (buddy_ready)
    {
        return;
    }

    /*
     * 從 DTB 決定實際 DRAM 起點與大小，
     */
    allocator_fdt = fdt;
    if (!fdt || fdt_get_memory_range(fdt, &mem_base, &mem_size) < 0)
    {
        uart_puts("[Mem] allocator init failed: cannot read /memory\n");
        return;
    }

    aligned_base = align_up_to_page(mem_base);
    aligned_end = align_down_to_page(mem_base + mem_size);
    if (aligned_end <= aligned_base)
    {
        uart_puts("[Mem] allocator init failed: invalid memory range\n");
        return;
    }

    //log_memory_region("memory", mem_base, mem_size);
    //log_memory_region("managed", aligned_base, aligned_end - aligned_base);

    managed_size = aligned_end - aligned_base;
    if (managed_size > max_size)
    {
        managed_size = max_size;
    }

    buddy_allocator.base_addr = aligned_base;
    buddy_allocator.page_count = managed_size / PAGE_SIZE;
    buddy_allocator.frames = NULL;

    /* 先標記保留區，再依剩餘區段重建 free lists */
    reset_dynamic_allocators();
    memset(reserved_page_map, 0, sizeof(reserved_page_map)); // 先將所有 frame 初始化為未保留
    startup_alloc_cursor = buddy_allocator.base_addr;

    reserve_memory_regions_from_fdt(fdt);

    frame_array_size = buddy_allocator.page_count * sizeof(struct frame);
    frame_array = startup_alloc(frame_array_size);
    if (frame_array == NULL)
    {
        uart_puts("[Mem] allocator init failed: cannot allocate frame array\n");
        return;
    }

    buddy_allocator.frames = (struct frame *)frame_array;
    //log_memory_region("frame_array",
    //                  (unsigned long)frame_array,
    //                  frame_array_size);
    buddy_build_free_areas();
    //log_free_list_state("init");
    buddy_ready = 1;
}

/* 第一次使用 allocator 時才做全域初始化，避免開機流程太早碰 allocator。 */
static void buddy_lazy_init(void)
{
    if (!buddy_ready)
    {
        mem_allocator_init(allocator_fdt);
    }
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

    page_idx = addr_to_frame_idx(&buddy_allocator, VA_TO_PA(page_addr));  // buddy_alloc 回 VA，需轉 PA
    chunk_size = chunk_pools[pool_index].chunk_size;
    chunk_count = PAGE_SIZE / chunk_size;

    pool_page_meta[page_idx].pool_index = pool_index;

    for (i = chunk_count; i > 0; i--)
    {
        chunk = (struct chunk *)(page_addr + (unsigned long)(i - 1) * chunk_size);
        chunk->next = chunk_pools[pool_index].free_list;
        chunk_pools[pool_index].free_list = chunk;
    }

    //log_pool_grow_event(page_idx, page_addr, chunk_size, chunk_count);

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
    //log_chunk_event("Allocate", (void *)chunk, chunk_pools[pool_index].chunk_size);

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
    struct chunk *cur;

    addr = VA_TO_PA((unsigned long)ptr);  // ptr 是 VA，轉 PA 才能和 base_addr 比較
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
        /*
         * 位址落在 chunk page 範圍內但未對齊到任何 chunk 邊界，
         * 這是非法指標。回傳 -1 讓 free() 繼續走 buddy_free()，
         * 由 buddy_free() 的指標合法性檢查印出明確的錯誤訊息。
         */
        uart_puts("[Mem] Free error: pointer is not a chunk base\n");
        return -1;
    }

    /* Double-free 偵測：掃描 free list 確認 ptr 尚未在其中。 */
    for (cur = chunk_pools[pool_index].free_list; cur != NULL; cur = cur->next)
    {
        if (cur == (struct chunk *)ptr)
        {
            uart_puts("[Mem] Double free detected for chunk at ");
            uart_hex(addr);
            uart_puts("\n");
            return 0;
        }
    }

    chunk = (struct chunk *)ptr;
    chunk->next = chunk_pools[pool_index].free_list;
    chunk_pools[pool_index].free_list = chunk;
    //log_chunk_event("Free", ptr, chunk_size);

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

    //log_free_list_state("init");
}

/*
 * 配置一個指定 order 的 block。
 * 流程是：
 * 1. 找到第一個可用的大 block。
 * 2. 從 free list 拿掉它。
 * 3. 一路往下 split，保留左半邊、把右半邊放回 free list。
 * 4. 最後把目標 block 標成 allocated 並回傳起始位址。
 */
void * buddy_alloc(unsigned int order)
{
    unsigned long idx;
    unsigned int cur_order;
    struct frame *frame;

    buddy_lazy_init();
    if (!buddy_ready)
    {
        return NULL;
    }

    if (order > MAX_ORDER) return NULL;

    for(cur_order = order; cur_order <= MAX_ORDER; cur_order++){
        if(!(list_empty(&buddy_allocator.free_area[cur_order]))) break;
    };

    if (cur_order > MAX_ORDER) return NULL;

    // 拿到這個 free block 的 head frame
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
        set_block_head(&buddy_allocator, idx, cur_order, 0); // 左半邊標記 is_free = 0
        free_area_add(&buddy_allocator, buddy_idx, cur_order); // 右半邊掛回 free_area[order]
        //log_split_redundant_block(buddy_idx, cur_order);
    }

    /*
     * 把最終分配出的 block 標成 allocated。
     * 只更新 head frame（O(1)），不標記其他 frames，
     * 以符合 Advanced Exercise 要求的 O(log n)。
     */
    set_block_head(&buddy_allocator, idx, order, 0);
    //log_page_alloc_event(idx, order);
    //log_free_list_state("allocate");

    return (void *)PA_TO_VA(buddy_allocator.base_addr + idx * PAGE_SIZE);  // 回傳 VA
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
    unsigned long addr = VA_TO_PA((unsigned long)ptr);  // 傳入 VA，轉回 PA 做內部計算
    unsigned long idx;
    unsigned long buddy_idx;
    unsigned long old_idx;
    unsigned long retired_idx;
    unsigned int cur_order;

    struct frame *frame;

    if (ptr == NULL) return;

    buddy_lazy_init();
    if (!buddy_ready)
    {
        return;
    }

    if (addr < buddy_allocator.base_addr ||
        addr >= frame_idx_to_addr(&buddy_allocator, buddy_allocator.page_count) ||
        (addr - buddy_allocator.base_addr) % PAGE_SIZE != 0) // 必須對齊 page
    {
        uart_puts("[Page] Free ignored: invalid pointer\n");
        return;
    }

    idx = addr_to_frame_idx(&buddy_allocator, addr);
    frame = &buddy_allocator.frames[idx]; // O(1)

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

        //log_buddy_found(idx, buddy_idx, cur_order);

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
    //log_page_free_event(addr, idx, cur_order);
    //log_free_list_state("free");
}

/* 對外的 byte-based 配置介面，內部轉成 buddy order 後交給 buddy_alloc。 */
void *allocate(unsigned long size)
{
    if (size == 0)
    {
        return NULL;
    }

    buddy_lazy_init();
    if (!buddy_ready)
    {
        return NULL;
    }

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
    if (!buddy_ready)
    {
        return;
    }

    if (pool_free_chunk(ptr) == 0)
    {
        return;
    }

    buddy_free(ptr);
}

/* ------ LOG ------*/

/* 輸出 block 的 page 範圍，方便 demo 時解釋 block 邊界。 */
// static void log_block_range(unsigned long idx, unsigned int order)
// {
//     uart_puts("Range of pages: [");
//     uart_dec(idx);
//     uart_puts(", ");
//     uart_dec(idx + block_pages(order) - 1);
//     uart_puts("]");
// }

// static void log_free_area_add(unsigned long idx, unsigned int order)
// {
//     uart_puts("[+] Add");
//     uart_puts(" page ");
//     uart_dec(idx);
//     uart_puts(" to order ");
//     uart_dec(order);
//     uart_puts(". ");
//     log_block_range(idx, order);
//     uart_puts("\n");
// }

// static void log_free_area_remove(unsigned long idx, unsigned int order)
// {
//     uart_puts("[-] Remove");
//     uart_puts(" page ");
//     uart_dec(idx);
//     uart_puts(" from order ");
//     uart_dec(order);
//     uart_puts(". ");
//     log_block_range(idx, order);
//     uart_puts("\n");
// }

// static void log_next_page_addr(unsigned int order)
// {
//     struct frame *next_frame;

//     if (list_empty(&buddy_allocator.free_area[order]))
//     {
//         uart_puts("none");
//         return;
//     }

//     next_frame = list_first_entry(&buddy_allocator.free_area[order],
//                                   struct frame,
//                                   free_list);
//     uart_hex(frame_idx_to_addr(&buddy_allocator,
//                                (unsigned long)(next_frame - buddy_allocator.frames)));
// }

// static void log_page_alloc_event(unsigned long idx, unsigned int order)
// {
//     uart_puts("[Page] Allocate ");
//     uart_hex(frame_idx_to_addr(&buddy_allocator, idx));
//     uart_puts(" at order ");
//     uart_dec(order);
//     uart_puts(", page ");
//     uart_dec(idx);
//     uart_puts(". Next address at order ");
//     uart_dec(order);
//     uart_puts(": ");
//     log_next_page_addr(order);
//     uart_puts("\n");
// }

// static void log_page_free_event(unsigned long addr, unsigned long idx, unsigned int order)
// {
//     uart_puts("[Page] Free ");
//     uart_hex(addr);
//     uart_puts(" and add back to order ");
//     uart_dec(order);
//     uart_puts(", page ");
//     uart_dec(idx);
//     uart_puts(". Next address at order ");
//     uart_dec(order);
//     uart_puts(": ");
//     log_next_page_addr(order);
//     uart_puts("\n");
// }

// static void log_buddy_found(unsigned long idx, unsigned long buddy_idx, unsigned int order)
// {
//     uart_puts("[*] Buddy found! buddy idx: ");
//     uart_dec(buddy_idx);
//     uart_puts(" for page ");
//     uart_dec(idx);
//     uart_puts(" with order ");
//     uart_dec(order);
//     uart_puts("\n");
// }

// static void log_memory_region(const char *label,
//                               unsigned long start,
//                               unsigned long size)
// {
//     unsigned long end;

//     uart_puts("[Mem] ");
//     uart_puts(label);
//     uart_puts(": ");

//     if (size == 0)
//     {
//         uart_puts("none\n");
//         return;
//     }

//     end = start + size;
//     if (end < start)
//     {
//         end = ~0UL;
//     }

//     uart_puts("[");
//     uart_hex(start);
//     uart_puts(", ");
//     uart_hex(end);
//     uart_puts("), size=");
//     uart_hex(size);
//     uart_puts("\n");
// }

// static void log_reserve_event(unsigned long start,
//                               unsigned long end,
//                               unsigned long first_page,
//                               unsigned long last_page)
// {
//     uart_puts("[Reserve] Reserve address [");
//     uart_hex(start);
//     uart_puts(", ");
//     uart_hex(end);
//     uart_puts("). Range of pages: [");
//     uart_dec(first_page);
//     uart_puts(", ");
//     uart_dec(last_page);
//     uart_puts(")\n");
// }

// static void log_chunk_event(const char *action, void *ptr, unsigned int chunk_size)
// {
//     uart_puts("[Chunk] ");
//     uart_puts(action);
//     uart_puts(" ");
//     uart_hex((unsigned long)ptr);
//     uart_puts(" at chunk size ");
//     uart_dec(chunk_size);
//     uart_puts("\n");
// }

// static void log_pool_grow_event(unsigned long page_idx,
//                                 unsigned long page_addr,
//                                 unsigned int chunk_size,
//                                 unsigned int chunk_count)
// {
//     uart_puts("[Pool] Grow page ");
//     uart_dec(page_idx);
//     uart_puts(" @ ");
//     uart_hex(page_addr);
//     uart_puts(" chunk_size ");
//     uart_dec(chunk_size);
//     uart_puts(" count ");
//     uart_dec(chunk_count);
//     uart_puts("\n");
// }

// static void log_free_list_state(const char *action)
// {
//     unsigned int order;

//     uart_puts("[Buddy] Free list blocks after ");
//     uart_puts(action);
//     uart_puts(": ");

//     for (order = 0; order <= MAX_ORDER; order++)
//     {
//         if (order > 0)
//         {
//             uart_puts(", ");
//         }

//         uart_puts("order ");
//         uart_dec(order);
//         uart_puts(" = ");
//         uart_dec(buddy_allocator.free_area_blocks[order]);
//     }

//     uart_puts("\n");
// }
