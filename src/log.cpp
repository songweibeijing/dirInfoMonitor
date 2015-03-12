#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include "log.h"
#include "util.h"
#include "config.h"

extern config g_config;

#define PERSIST_LOG "/var/log/dircounter.log"
#define STRLEN 256
#define MAX_BUFFER_SIZE 1024*1024*5
#define TIME_OUT 100

static int level_log = LOG_NOTICE;
static int check_file = 0;
static volatile int log_file = -1;
volatile int log_fd_invalid = 0;
const int log_cfg_num = 8;

static pthread_mutex_t g_locks[2];
static int g_index = 0;
char *g_buffer[2] = {NULL, NULL};
int g_buffer_pos[2] = {0, 0};

log_conf log_cfg[10] =
{
    {(char *)"LOG_EMERG",       LOG_EMERG},
    {(char *)"LOG_ALERT",       LOG_ALERT},
    {(char *)"LOG_CRIT",        LOG_CRIT},
    {(char *)"LOG_ERR",         LOG_ERR},
    {(char *)"LOG_WARN",        LOG_WARN},
    {(char *)"LOG_NOTICE",      LOG_NOTICE},
    {(char *)"LOG_INFO",        LOG_INFO},
    {(char *)"LOG_DEBUG",       LOG_DEBUG},
};

char *logd_event_desc[] =
{
    (char *)"dcsys",
    NULL,
};

int debug_init(void)
{
    return 0;
}

int open_log_file(const char *file)
{
    if (NULL == file || strlen(file) == 0)
    {
        return -1;
    }

    int fd = open(file, O_CREAT | O_WRONLY | O_APPEND, 0640);
    if (fd < 0)
    {
        char file_tmp[1024] = {0};
        strncpy(file_tmp, file, 1024);
        char *dir = strrchr(file_tmp, '/');
        if (NULL == dir)
        {
            printf("the file must be a dir\n");
            return -1;
        }
        //the dir path may be lost
        *dir = '\0';
        if (0 == make_dir_recusive(file_tmp, 0644))
        {
            printf("the mkdir recursive failed, the dir is %s\n", dir);
            return -1;
        }
        fd = open(file, O_CREAT | O_WRONLY | O_APPEND, 0640);
    }
    if (fd < 0)
    {
        return -1;
    }

    //release old log file handler
    if (log_file > 0)
    {
        close(log_file);
    }
    log_file = fd;
    log_fd_invalid = 0;
    return 0;
}

void *async_write(void *argv)
{
    pthread_detach(pthread_self());

    struct timeval interval;
    while (1)
    {
        interval.tv_sec = 0;
        interval.tv_usec = 1000 * TIME_OUT;
        select(0, NULL, NULL, NULL, &interval);

        check_file++;
        if (log_fd_invalid != 1 && check_file > 100) //10s
        {
            check_file = 0;
            if (access(PERSIST_LOG, R_OK) != 0) //not exist
            {
                log_fd_invalid = 1;
            }
        }

        if (1 == log_fd_invalid)
        {
            if (open_log_file(PERSIST_LOG) < 0)
            {
                printf("open log file failed\n");
                continue; //logdaemon without log
            }
        }

        int index = __sync_fetch_and_xor(&g_index, 1);

        pthread_mutex_lock(&g_locks[index]);

        int wpos = g_buffer_pos[index];
        if (wpos > 0 && my_write(log_file, (char *)g_buffer[index], wpos) != 0)
        {
            printf("write error\n");
            log_fd_invalid = 1;
        }
        g_buffer_pos[index] = 0;

        pthread_mutex_unlock(&g_locks[index]);
    }
    return NULL;
}

void up_loglevel()
{
    int level = level_log;
    level++;
    if (level <= 7)
    {
        level_log = level;
    }
}

void down_loglevel()
{
    int level = level_log;
    level--;
    if (level >= 0)
    {
        level_log = level;
    }
}

int parser_log_configure(void)
{
    int i;
    for (i = 0; i < log_cfg_num; i++)
    {
        if (strcasestr(g_config.loglevel, log_cfg[i].cfg_key) != NULL)
        {
            level_log = log_cfg[i].cfg_shift;
            break;
        }
    }

    printf("level_log = 0X%x\n", level_log);
    return 0;
}

void init_persist_log(void)
{
    int ret = 0;
    int i = 0;
    pthread_t p_async_writer;

    log_fd_invalid = 1;
    ret = parser_log_configure();
    if (ret < 0)
    {
        printf("parser_log_configure failed\n");
        /* set the default value */
        level_log = LOG_NOTICE;
    }
    if (open_log_file(PERSIST_LOG) < 0)
    {
        printf("open log file failed\n");
        return; //just return, logdaemon without log
    }

    g_index = 0;
    while (i < 2)
    {
        pthread_mutex_init(&g_locks[i], NULL);
        g_buffer[i] = (char *)malloc(MAX_BUFFER_SIZE);
        if (NULL == g_buffer[i])
        {
            goto failed;
        }
        memset(g_buffer[i], 0, MAX_BUFFER_SIZE);
        g_buffer_pos[i] = 0;
        i++;
    }

    if (pthread_create(&p_async_writer, NULL, async_write, NULL) < 0)
    {
        goto failed;
    }

    return;

failed:
    if (g_buffer[0] != NULL)
    {
        free(g_buffer[0]);
    }
    if (g_buffer[1] != NULL)
    {
        free(g_buffer[1]);
    }
    log_fd_invalid = 1;
    return; //logdaemon without log
}

static int _persist_log(int level, char *stamp, char *text)
{
    int index = 0;
    int wpos = 0;
    char str_buf[4096] = {0};
    int str_buf_len = 0;

#ifdef DETAILED_LOGS
    snprintf(str_buf, sizeof(str_buf), "%s %s", stamp, text);
#else
    snprintf(str_buf, sizeof(str_buf), "%s LEVEL[%d] %s", stamp, level, text);
#endif

    index = g_index;

    pthread_mutex_lock(&g_locks[index]);

    wpos = g_buffer_pos[index];
    str_buf_len = strlen(str_buf);
    if (wpos + str_buf_len < MAX_BUFFER_SIZE)
    {
        strncpy(g_buffer[index] + wpos, (char *)str_buf, str_buf_len);
        g_buffer_pos[index] += str_buf_len;
    }
    else
    {
        if (wpos > 0 && my_write(log_file, (char *)g_buffer[index], wpos) != 0)
        {
            log_fd_invalid = 1;
        }
        g_buffer_pos[index] = 0;
        strncpy(g_buffer[index], str_buf, str_buf_len);
        g_buffer_pos[index] += str_buf_len;
    }

    pthread_mutex_unlock(&g_locks[index]);

    return 0;
}

/*be careful this is NOT signal safe, don't call it in the signal handler*/
int persist_log(int level, const char *format, ...)
{
#define LOG_TEXT_LEN 4096
    va_list arglist;
    char text[LOG_TEXT_LEN], stamp[STRLEN];
    int ret = 0;

    if (1 == log_fd_invalid)
    {
        return -1;
    }

    if (level > level_log || level < 0 || level > 7)
    {
        return 0;
    }

    memset(text, 0, sizeof(text));
    memset(stamp, 0, sizeof(stamp));

    va_start(arglist, format);
    vsnprintf(text, LOG_TEXT_LEN, format, arglist);
    va_end(arglist);

    get_timestamp(level, stamp);
    ret = _persist_log(level, stamp, text);
    if (ret > 0)
    {
        return 0;
    }
    else
    {
        return -1;
    }
}

