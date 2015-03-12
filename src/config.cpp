#include "header.h"
#include "headercxx.h"
#include "config.h"
#include "util.h"

static struct command config_commands[] =
{
    {
        "LOG_LEVEL",
        config_set_string,
        offsetof(struct config, loglevel)
    },

    {
        "log_file",
        config_set_string,
        offsetof(struct config, logfile)
    },

    {
        "db_dir",
        config_set_string,
        offsetof(struct config, db_dir)
    },

    {
        "db_name",
        config_set_string,
        offsetof(struct config, db_name)
    },

    {
        "default_monitor_dir",
        config_set_string,
        offsetof(struct config, default_monitor_file)
    },

    {
        "max_memory_threshold",
        config_set_int,
        offsetof(struct config, max_memory)
    },

    {
        "dump_interval",
        config_set_int,
        offsetof(struct config, dump_interval)
    },

    {
        "check_interval",
        config_set_int,
        offsetof(struct config, check_interval)
    },

    {
        "check_one_folder_interval",
        config_set_int,
        offsetof(struct config, check_one_folder_interval)
    },

    null_command
};

void print_config(config *cfg);

int parse_file(char *buf, void *res)
{
    config *cfg = (config *)res;
    char *value = strchr(buf, '=');
    if (value == NULL)
    {
        return -1;
    }
    *value++ = '\0';
    char *key = buf;

    struct command *cmd;
    for (cmd = config_commands; strlen(cmd->name) != 0; cmd++)
    {
        int rv;

        if (strncmp(key, cmd->name, strlen(key)) != 0)
        {
            continue;
        }

        rv = cmd->set(cfg, cmd, value);
        if (rv != 0)
        {
            printf("config: directive %s : %s\n", key, value);
            return -1;
        }

        return 0;
    }

    return -1;
}

int init_config(const char *file, config *cfg)
{
    assert(file != NULL);

    int ret = process_file(file, parse_file, (void *)cfg);
    if (ret != 0)
    {
        cerr << "parse config file error" << endl;
        return -1;
    }

    if (cfg->max_memory <= 0 || cfg->max_memory >= 3096)
    {
        cfg->max_memory = 1024;
    }
    cfg->max_memory *= 1024 * 1024;

    if (cfg->check_interval <= 10)
    {
        cfg->check_interval = 300;
    }
    if (cfg->check_one_folder_interval <= 10)
    {
        cfg->check_one_folder_interval = 30;
    }


    print_config(cfg);
    return 0;
}

int config_set_string(struct config *cf, struct command *cmd, void *value)
{
    uint8_t *p;
    char *str, **strp;

    p = (uint8_t *)cf;
    strp = (char **)(p + cmd->offset);

    int len = strlen((char *)value) + 1;
    str = (char *)malloc(len * sizeof(char));
    if (str == NULL)
    {
        printf("malloc error\n");
        return -1;
    }
    memset(str, 0, len);

    strncpy(str, (char *)value, len);
    *strp = str;

    return 0;
}

int config_set_short(struct config *cf, struct command *cmd, void *value)
{
    uint8_t *p;
    short num, *np;

    p = (uint8_t *)cf;
    np = (short *)(p + cmd->offset);

    num = atoi((char *)value);
    *np = num;

    return 0;
}

int config_set_double(struct config *cf, struct command *cmd, void *value)
{
    uint8_t *p;
    double num, *np;

    p = (uint8_t *)cf;
    np = (double *)(p + cmd->offset);

    num = atof((char *)value);
    *np = num;

    return 0;
}

int config_set_int(struct config *cf, struct command *cmd, void *value)
{
    uint8_t *p;
    int num, *np;

    p = (uint8_t *)cf;
    np = (int *)(p + cmd->offset);

    num = atoi((char *)value);
    *np = num;

    return 0;
}

int config_set_bool(struct config *cf, struct command *cmd, void *value)
{
    uint8_t *p;
    bool *bp;

    p = (uint8_t *)cf;
    bp = (bool *)(p + cmd->offset);

    char true_str[6] = "true";
    char false_str[6] = "false";

    if (strncmp((char *)value, true_str, strlen(true_str)) == 0)
    {
        *bp = 1;
    }
    else if (strncmp((char *)value, false_str, strlen(false_str)) == 0)
    {
        *bp = 0;
    }
    else
    {
        return -1;
    }

    return 0;
}

void print_config(config *cfg)
{
    command *pcommand = config_commands;
    while (pcommand->set != NULL)
    {
        uint8_t *p = (uint8_t *)cfg + pcommand->offset;

        if (pcommand->set == config_set_string)
        {
            printf("%s = %s\n", pcommand->name, *(char **)p);
        }
        else if (pcommand->set == config_set_short)
        {
            printf("%s = %d\n", pcommand->name, *(short *)p);
        }
        else if (pcommand->set == config_set_double)
        {
            printf("%s = %f\n", pcommand->name, *(double *)p);
        }
        else if (pcommand->set == config_set_int)
        {
            printf("%s = %d\n", pcommand->name, *(int *)p);
        }

        pcommand++;
    }

    return;
}

