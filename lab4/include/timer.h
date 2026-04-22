#ifndef TIMER_H
#define TIMER_H

void add_timer(void (*callback)(void *), void *arg, unsigned long sec);

#endif
