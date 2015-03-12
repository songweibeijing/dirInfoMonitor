#ifndef _INOTIFY_PROCESS_H
#define _INOTIFY_PROCESS_H

#include "header.h"
#include "monitor_dir.h"

#define ADD 1
#define DEL 0

#define EXIT_OK 0
#define EXIT_ERROR 1
#define EXIT_TIMEOUT 2
#define CONFIG_FILE "/usr/local/etc/dircounter.conf"

int init_notify_fs(const char *fromfile);
int add_notify_dir(const char *dir, int events, int level, char **exclude_list);

int get_monitor_dir_from_config(const char *configfile, monitor_dirs *md);
void do_self_test();

#endif
