#include "mem_allocator.h"
#include "mem_allocator_test.h"
#include "uart.h"

#define MAX_ALLOC_SIZE ((1UL << MAX_ORDER) * PAGE_SIZE)

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
