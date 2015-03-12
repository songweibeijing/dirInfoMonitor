#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <regex.h>
#include <getopt.h>

#include "header.h"
#include "headercxx.h"
#include "inotify_process.h"
#include "sig.h"
#include "log.h"
#include "config.h"
#include "kv.h"
#include "monitor_dir.h"
#include "bio.h"
#include "dump.h"

#define PID_FILE "/var/run/dircounter.pid"

config g_config;
int g_events;
bdb_info *g_hash_db = NULL; //to store the kv if the memory usage is very large.
int g_pidfd = 0;

extern void do_self_test();

static int lock_reg(int fd, int cmd, int type, off_t offset, int whence, off_t len)
{
    struct flock lock;
    lock.l_type = type;
    lock.l_start = offset;
    lock.l_whence = whence;
    lock.l_len = len;

    return (fcntl(fd, cmd, &lock));
}

int write_lock_try(int fd, off_t offset, int whence, off_t len)
{
    return lock_reg(fd, F_SETLK, F_WRLCK, offset, whence, len);
}

int unlock(int fd, off_t offset, int whence, off_t len)
{
    return lock_reg(fd, F_SETLK, F_UNLCK, offset, whence, len);
}

static void save_pid(const pid_t pid, const char *pid_file)
{
    char line[1024] = {0};
    if (pid_file == NULL)
    {
        return;
    }

    g_pidfd = open(pid_file, O_RDWR | O_CREAT, 0x644);
    if (g_pidfd <= 0)
    {
        printf("Could not open the pid file %s for writing\n", pid_file);
        return;
    }

    if (write_lock_try(g_pidfd, 0, SEEK_SET, 0) < 0)
    {
        if (errno == EACCES || errno == EAGAIN)
        {
            printf("unable to lock %s, another dircounter program already running\n", pid_file);
            return;
        }
        else
        {
            printf("unable to lock %s\n", pid_file);
        }
    }
    printf("Succ to get the lock\n");
    snprintf(line, sizeof(line), "%d", pid);
    ftruncate(g_pidfd, 0);
    write(g_pidfd, line, strlen(line));
    fsync(g_pidfd);
}

static void remove_pidfile(const char *pid_file)
{
    if (pid_file == NULL)
    {
        return;
    }

    unlock(g_pidfd, 0, SEEK_SET, 0);
    if (unlink(pid_file) != 0)
    {
        printf("Could not remove the pid file %s", pid_file);
    }
}

static void usage(void)
{
    printf("\n");

    printf("usage: dircounterd [-h-H]\n");
    printf("\tkill -INT `cat /var/run/logdaemon.pid\n");
    printf("\t\tkill the dircounterd\n");
    printf("\tkill -USR1 `cat /var/run/logdaemon.pid\n");
    printf("\t\toutput debug information in log\n");
    printf("\tkill -USR2 `cat /var/run/logdaemon.pid\n");
    printf("\t\trotate the logfile\n");
    printf("\tkill -TTIN `cat /var/run/logdaemon.pid`\n");
    printf("\t\tup loglevel, get more details\n");
    printf("\tkill -TTOU `cat /var/run/logdaemon.pid`\n");
    printf("\t\tdown loglevel, get less details\n");

    printf("\n");
}

#define PEXIT(str) do{\
        if(ret!=0)\
        {\
            printf("%s\n", str);\
            exit(-1);\
        }\
    }while(0)

#define PEXIT_EX(str) do{\
        if(ret!=0)\
        {\
            debug_sys(LOG_ERR, "%s\n", str);\
            exit(-1);\
        }\
    }while(0)

int main(int argc, char **argv)
{
    int ret = 0;
    char c;
    char internal_path[1024] = {0};

    srand(time(NULL));

    /* arguments process */
    while ((c = getopt(argc, argv, "hH")) != -1)
    {
        switch (c)
        {
            case 'h':
                usage();
                exit(0);

            case 'H':
                usage();
                exit(0);

            default:
                usage();
                exit(0);
        }
    }

    ret = signal_init();
    PEXIT("init signal error\n");

    /* save the pid */
    save_pid(getpid(), PID_FILE);

    ret = init_config(CONFIG_FILE, &g_config);
    PEXIT("init config error\n");

    debug_init();
    init_persist_log();

    debug_sys(LOG_NOTICE, "dircounter begin to init\n");

    mem_object_init();
    snprintf(internal_path, 1024, "%s", g_config.db_dir);
    g_hash_db = init_kv_storage(internal_path, (char *)"tmp", HASH, 1, 1);
    if (g_hash_db == NULL)
    {
        debug_sys(LOG_ERR, "init kv storage error\n");
        exit(-1);
    }

    ret = bio_init();
    PEXIT_EX("init bio error\n");

    ret = init_notify_fs(g_config.default_monitor_file);
    PEXIT_EX("init notify fs error\n");

    ret = dump_thread_create();
    PEXIT_EX("init dump thread error\n");
    debug_sys(LOG_NOTICE, "dircounter starts\n");

    while (1)
    {
        pause();
    }

    debug_sys(LOG_NOTICE, "dircounter stops\n");
    remove_pidfile(PID_FILE);

    return EXIT_OK;
}

