#include "mem_allocator.h"
#include "mem_allocator_test.h"
#include "uart.h"

#define MAX_ALLOC_SIZE ((1UL << MAX_ORDER) * PAGE_SIZE)
#define BUDDY_MERGE_TEST_PAGES 64

static int are_order0_buddies(void *lhs, void *rhs)
{
    unsigned long lhs_idx = ((unsigned long)lhs) / PAGE_SIZE;
    unsigned long rhs_idx = ((unsigned long)rhs) / PAGE_SIZE;

    return (lhs_idx ^ 1UL) == rhs_idx;
}

void run_mem_allocator_test(void)
{
    void *ptr1;
    void *ptr2;
    void *ptr3;
    void *ptr4;
    void *merge_ptr1;
    void *merge_ptr2;
    void *kmem_ptr1;
    void *kmem_ptr2;
    void *kmem_ptr3;
    void *kmem_ptr4;
    void *kmem_ptr5;
    void *kmem_ptr6;
    void *kmem_ptr7;
    void *kmem_ptr[100];
    int i;

    uart_puts("Testing memory allocation...\n");
    ptr1 = allocate(4000);
    ptr2 = allocate(8000);
    ptr3 = allocate(4000);
    ptr4 = allocate(4000);

    free(ptr1);
    free(ptr2);
    free(ptr3);
    free(ptr4);

    uart_puts("Testing merged head invalidation...\n");
    merge_ptr1 = allocate(PAGE_SIZE);
    merge_ptr2 = allocate(PAGE_SIZE);
    free(merge_ptr1);
    free(merge_ptr2);
    free(merge_ptr2);

    uart_puts("Testing dynamic allocator...\n");
    kmem_ptr1 = allocate(16);
    kmem_ptr2 = allocate(32);
    kmem_ptr3 = allocate(64);
    kmem_ptr4 = allocate(128);

    free(kmem_ptr1);
    free(kmem_ptr2);
    free(kmem_ptr3);
    free(kmem_ptr4);

    kmem_ptr5 = allocate(16);
    kmem_ptr6 = allocate(32);

    free(kmem_ptr5);
    free(kmem_ptr6);

    for (i = 0; i < 100; i++)
    {
        kmem_ptr[i] = allocate(128);
    }

    for (i = 0; i < 100; i++)
    {
        free(kmem_ptr[i]);
    }

    kmem_ptr7 = allocate(MAX_ALLOC_SIZE + 1);
    if (kmem_ptr7 == NULL)
    {
        uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\n");
    }
    else
    {
        uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\n");
        free(kmem_ptr7);
    }

    uart_puts("Memory allocation test finished.\n");
}

void run_buddy_merge_test(void)
{
    void *pages[BUDDY_MERGE_TEST_PAGES];
    int alloc_count = 0;
    int buddy_a = -1;
    int buddy_b = -1;
    int i;

    uart_puts("Testing buddy merge detection...\n");

    for (i = 0; i < BUDDY_MERGE_TEST_PAGES; i++)
    {
        int j;

        pages[i] = allocate(PAGE_SIZE);
        if (pages[i] == NULL)
        {
            uart_puts("Buddy merge test stopped early: allocate(PAGE_SIZE) returned NULL\n");
            break;
        }

        alloc_count++;
        for (j = 0; j < i; j++)
        {
            if (are_order0_buddies(pages[j], pages[i]))
            {
                buddy_a = j;
                buddy_b = i;
                break;
            }
        }

        if (buddy_a >= 0)
        {
            break;
        }
    }

    if (buddy_a < 0)
    {
        uart_puts("Buddy merge test could not find an order-0 buddy pair.\n");
        for (i = 0; i < alloc_count; i++)
        {
            free(pages[i]);
        }
        return;
    }

    uart_puts("Buddy pair chosen by test: ");
    uart_hex((unsigned long)pages[buddy_a]);
    uart_puts(" and ");
    uart_hex((unsigned long)pages[buddy_b]);
    uart_puts("\n");

    /*
     * 先 free 其中一頁，再 free 它的 buddy。
     * 第二次 free 時 buddy allocator 應該會印出 [*] Buddy found!。
     */
    free(pages[buddy_a]);
    free(pages[buddy_b]);

    for (i = 0; i < alloc_count; i++)
    {
        if (i == buddy_a || i == buddy_b)
        {
            continue;
        }

        free(pages[i]);
    }

    uart_puts("Buddy merge test finished.\n");
}

// void run_mem_allocator_test(void)
// {
//     void *ptr1;
//     uart_puts("Testing memory allocation...\n");
//     ptr1 = allocate(4000);
//     free(ptr1);

//     uart_puts("Memory allocation test finished.\n");
// }
