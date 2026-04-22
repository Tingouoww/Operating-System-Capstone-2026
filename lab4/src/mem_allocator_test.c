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
    /***************** Case 2 *****************/

    uart_puts("\n===== Part 1 =====\n");

    void *p1 = allocate(129);
    free(p1);

    uart_puts("\n=== Part 1 End ===\n");

    uart_puts("\n===== Part 2 =====\n");

    // Allocate all blocks at order 0, 1, 2 and 3
    int NUM_BLOCKS_AT_ORDER_0 = 0;  // Need modified
    int NUM_BLOCKS_AT_ORDER_1 = 0;
    int NUM_BLOCKS_AT_ORDER_2 = 0;
    int NUM_BLOCKS_AT_ORDER_3 = 0;

    void *ps0[NUM_BLOCKS_AT_ORDER_0];
    void *ps1[NUM_BLOCKS_AT_ORDER_1];
    void *ps2[NUM_BLOCKS_AT_ORDER_2];
    void *ps3[NUM_BLOCKS_AT_ORDER_3];
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i) {
        ps0[i] = allocate(4096);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i) {
        ps1[i] = allocate(8192);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i) {
        ps2[i] = allocate(16384);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i) {
        ps3[i] = allocate(32768);
    }

    uart_puts("\n-----------\n");

    long MAX_BLOCK_SIZE = PAGE_SIZE * (1 << MAX_ORDER);

    /* **DO NOT** uncomment this section */
    void *c1, *c2, *c3, *c4, *c5, *c6, *c7, *c8, *p2, *p3, *p4, *p6, *p7;

    p1 = allocate(4095);
    free(p1);                        // 4095
    p1 = allocate(4095);

    c1 = allocate(1000);
    c2 = allocate(1023);
    c3 = allocate(999);
    c4 = allocate(1010);
    free(c3);                        // 999
    c5 = allocate(989);  
    c3 = allocate(88);
    c6 = allocate(1001);
    free(c3);                        // 88
    c7 = allocate(2045);
    c8 = allocate(1);

    p2 = allocate(4096);
    free(c8);                        // 1
    p3 = allocate(16000);
    free(p1);                        // 4095
    free(c7);                        // 2045
    p4 = allocate(4097);
    p6 = allocate(MAX_BLOCK_SIZE);
    free(p2);                        // 4096
    free(p4);                        // 4097
    p7 = allocate(7197);

    free(p6);                        // MAX_BLOCK_SIZE
    free(p3);                        // 16000
    free(p7);                        // 7197
    free(c1);                        // 1000
    free(c6);                        // 1001
    free(c2);                        // 1023
    free(c5);                        // 989
    free(c4);                        // 1010


    uart_puts("\n-----------\n");

    // Free all blocks remaining
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i) {
        free(ps0[i]);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i) {
        free(ps1[i]);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i) {
        free(ps2[i]);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i) {
        free(ps3[i]);
    }

    uart_puts("\n=== Part 2 End ===\n");
    // void *ptr1;
    // void *ptr2;
    // void *ptr3;
    // void *ptr4;
    // // void *merge_ptr1;
    // // void *merge_ptr2;
    // void *kmem_ptr1;
    // void *kmem_ptr2;
    // void *kmem_ptr3;
    // void *kmem_ptr4;
    // void *kmem_ptr5;
    // void *kmem_ptr6;
    // void *kmem_ptr7;
    // void *kmem_ptr[100];
    // int i;

    // uart_puts("Testing memory allocation...\n");
    // ptr1 = allocate(4000);
    // ptr2 = allocate(8000);
    // ptr3 = allocate(4000);
    // ptr4 = allocate(4000);

    // free(ptr1);
    // free(ptr2);
    // free(ptr3);
    // free(ptr4);

    // // uart_puts("Testing merged head invalidation...\n");
    // // merge_ptr1 = allocate(PAGE_SIZE);
    // // merge_ptr2 = allocate(PAGE_SIZE);
    // // free(merge_ptr1);
    // // free(merge_ptr2);
    // // free(merge_ptr2);

    // uart_puts("Testing dynamic allocator...\n");
    // kmem_ptr1 = allocate(16);
    // kmem_ptr2 = allocate(32);
    // kmem_ptr3 = allocate(64);
    // kmem_ptr4 = allocate(128);

    // free(kmem_ptr1);
    // free(kmem_ptr2);
    // free(kmem_ptr3);
    // free(kmem_ptr4);

    // kmem_ptr5 = allocate(16);
    // kmem_ptr6 = allocate(32);

    // free(kmem_ptr5);
    // free(kmem_ptr6);

    // for (i = 0; i < 100; i++)
    // {
    //     kmem_ptr[i] = allocate(128);
    // }

    // for (i = 0; i < 100; i++)
    // {
    //     free(kmem_ptr[i]);
    // }

    // kmem_ptr7 = allocate(MAX_ALLOC_SIZE + 1);
    // if (kmem_ptr7 == NULL)
    // {
    //     uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\n");
    // }
    // else
    // {
    //     uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\n");
    //     free(kmem_ptr7);
    // }

    // uart_puts("Memory allocation test finished.\n");
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
