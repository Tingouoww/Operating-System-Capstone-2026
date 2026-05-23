#ifndef SIGNAL_H
#define SIGNAL_H
#include "pt_regs.h"

#define SIGTERM 15
#define MAX_SIGNALS 32
#define SIG_DFL 0 // 沒有設定 handler(kill process)

struct saved_signal_context{
    struct  pt_regs save_regs;
    unsigned long signal_stack;
};

#endif