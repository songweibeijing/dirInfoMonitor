#ifndef __SIGNAL_H__
#define __SIGNAL_H__

#include "header.h"

#define null_signal { 0, (char *)"", (char *)"", 0, NULL }

struct signal
{
    int  signo;
    char *signame;      //the name of signal
    char *actionstr;    //the description of action.
    int  flags;
    int (*handler)(int signo);
};

int signal_init(void);
void signal_deinit(void);

#endif
