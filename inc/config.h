#ifndef __CONFIG_H__
#define __CONFIG_H__

#include "header.h"
#define null_command { "", NULL, 0 }

typedef struct config
{
    int  dump_interval;

    //log
    char *logfile;
    char *loglevel;

    //db
    char *db_dir;
    char *db_name;
    char *default_monitor_file;

    //#memory threshold, in MBytes
    //#The mim value is 1024 MBytes.
    uint64_t max_memory;

    //check
    int  check_interval;
    int  check_one_folder_interval;
} config;

extern config g_config;

struct command
{
    char    name[1024];
    int (*set)(struct config *cf, struct command *cmd, void *data);
    int     offset;
};

int init_config(const char *file, config *cfg);

int config_set_string(struct config *cf, struct command *cmd, void *value);
int config_set_short(struct config *cf, struct command *cmd, void *value);
int config_set_double(struct config *cf, struct command *cmd, void *value);
int config_set_int(struct config *cf, struct command *cmd, void *value);

#endif
