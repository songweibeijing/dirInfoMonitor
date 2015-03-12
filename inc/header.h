#ifndef __HEADER_H__
#define __HEADER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <fcntl.h>
#include <error.h>
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <grp.h>
#include <pwd.h>
#include <assert.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <ctype.h>
#include <poll.h>
#include <sys/epoll.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <dirent.h>

#define MAX_PATH 1024
#define TMP_BUFSIZ 1024

#define NOOP    0
#define ADD     1
#define DELE    2

#define SUCC    0
#define ERROR   -1

#define NFOUND  0
#define FOUND   1

typedef struct action_item
{
    char *path;
    int64_t filesz;
    int64_t filenm;
    int action;
} action_item;

int init_notify_fs(const char *fromfile);
int add_notify_dir(const char *dir, int events, int level, char **exclude_list);
int add_notify_file(const char *file, int events);


//max entry of hash
#define MAX_INDEX 10240
#define SHM_DIRCOUNTER_INDEX    "/shm_dircounter_index"

//date file and its content
#define DATA_FILE               "/var/log/dircounter_data"

typedef struct fileinfo
{
    int64_t filesz;
    int64_t filenm;
} fileinfo;

typedef struct data_record
{
    char file[256];
    fileinfo fi;
} data_rec;

//shared memory entry structure
typedef struct record_index
{
    int version;            //reserved for upgrade
    uint64_t index;
    uint64_t cnt;
    int in_use;
} rc_index;

#define my_free(x) do{\
        if(x) \
        { \
            free(x);\
            x = NULL;\
        }\
    }while(0)

#endif
