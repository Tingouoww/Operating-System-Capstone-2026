#ifndef LIST_H
#define LIST_H

#include <stddef.h>

struct list_head
{
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

/* 
    get the struct for this entry
    (已知某個 struct 裡面的成員指標 ptr，反推出「整個 struct 的起始位址」)
    - ptr：指向某個欄位，例如 &frame->free_list
    - type：外層 struct 型別，例如 struct frame
    - member：那個欄位名稱，例如 free_list
*/
#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr)-offsetof(type, member)))

/* get the first element from a list */
#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new_node,
                              struct list_head *prev,
                              struct list_head *next)
{
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

/* add new node after head */
static inline void list_add(struct list_head *new_node, struct list_head *head)
{
    __list_add(new_node, head, head->next);
}

/* 
* add new node after the last node
* in doubly linked list, head->prev is the last and head->next is the first
*/
static inline void list_add_tail(struct list_head *new_node, struct list_head *head)
{
    __list_add(new_node, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = NULL;
    entry->prev = NULL;
}

/* check the list is empty or not*/
static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

#endif
