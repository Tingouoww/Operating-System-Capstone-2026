#ifndef TIMER_H
#define TIMER_H

extern unsigned long boot_seconds;

void add_timer(void (*callback)(void *), void *arg, unsigned long sec);
void timer_init(const void *fdt);
void timer_handle_irq(void);

#endif
