#ifndef TIMER_H
#define TIMER_H

int add_timer(void (*callback)(void *), void *arg, unsigned long sec);
int add_timer_ticks(void (*callback)(void *), void *arg, unsigned long ticks);
int add_timer_us(void (*callback)(void *), void *arg, unsigned long usec);
unsigned long timer_get_frequency(void);
unsigned long timer_us_to_ticks(unsigned long usec);
int timer_consume_preempt_pending(void);
void timer_init(const void *fdt);
void timer_handle_irq(void);

#endif
