#ifndef TASK_H
#define TASK_H

typedef void (*task_callback_t)(void *arg);

void add_task(task_callback_t callback, void *arg, int priority);
void run_tasks(void);

#endif
