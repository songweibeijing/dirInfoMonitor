/*
 * readdir accelerator
 *
 * (C) Copyright 2003, 2004, 2008 by Theodore Ts'o.
 *
 * 2008-06-08 Modified by Ross Boylan <RossBoylan stanfordalumni org>
 *    Added support for readdir_r and readdir64_r calls.  Note
 *     this has not been tested on anything other than GNU/Linux i386,
 *     and that the regular readdir wrapper will take slightly more
 *     space than Ted's original since it now includes a lock.
 *
 * Compile using the command:
 *
 * gcc -o spd_readdir.so -shared -fpic spd_readdir.c -ldl
 *
 * Use it by setting the LD_PRELOAD environment variable:
 *
 * export LD_PRELOAD=/usr/local/sbin/spd_readdir.so
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 *
 */

#define ALLOC_STEPSIZE  4096
#define MAX_DIRSIZE 0

#define DEBUG
/* Util we autoconfiscate spd_readdir... */
#define HAVE___SECURE_GETENV    1
#define HAVE_PRCTL      1
#define HAVE_SYS_PRCTL_H    1

#ifdef DEBUG
#define DEBUG_DIR(x)    {if (do_debug) { x; }}
#else
#define DEBUG_DIR(x)
#endif

#define _GNU_SOURCE
#define __USE_LARGEFILE64

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <dlfcn.h>
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#else
#define PR_GET_DUMPABLE 3
#endif
#include <pthread.h>

#include "spd_readdir.h"

struct dirent_s
{
    unsigned long long d_ino;
    long long d_off;
    unsigned short int d_reclen;
    unsigned char d_type;
    char *d_name;
};

struct dir_s
{
    DIR *dir;
    pthread_mutex_t lock; /* Mutex lock for this structure.  */
    int num;
    int max;
    struct dirent_s *dp;
    int pos;
    int fd;
    struct dirent ret_dir;
    struct dirent64 ret_dir64;
};

static int (*real_closedir)(DIR *dir) = 0;
static DIR *(*real_opendir)(const char *name) = 0;
static DIR *(*real_fdopendir)(int fd) = 0;
static struct dirent *(*real_readdir)(DIR *dir) = 0;
static struct dirent64 *(*real_readdir64)(DIR *dir) = 0;
static int (*real_readdir_r)(DIR *dir, struct dirent *entry,
                             struct dirent **result) = 0;
static int (*real_readdir64_r)(DIR *dir, struct dirent64 *entry,
                               struct dirent64 **result) = 0;
static off_t(*real_telldir)(DIR *dir) = 0;
static void (*real_seekdir)(DIR *dir, off_t offset) = 0;
static int (*real_dirfd)(DIR *dir) = 0;
static unsigned long max_dirsize = MAX_DIRSIZE;
static int num_open = 0;
#ifdef DEBUG
static int do_debug = 0;
#endif

static char *safe_getenv(const char *arg)
{
    if ((getuid() != geteuid()) || (getgid() != getegid()))
    {
        return NULL;
    }
#if HAVE_PRCTL
    if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0)
    {
        return NULL;
    }
#else
#if (defined(linux) && defined(SYS_prctl))
    if (syscall(SYS_prctl, PR_GET_DUMPABLE, 0, 0, 0, 0) == 0)
    {
        return NULL;
    }
#endif
#endif

#if HAVE___SECURE_GETENV
    return __secure_getenv(arg);
#else
    return getenv(arg);
#endif
}

static void setup_ptr()
{
    char *cp;

    real_opendir = dlsym(RTLD_NEXT, "opendir");
    real_fdopendir = dlsym(RTLD_NEXT, "fdopendir");
    real_closedir = dlsym(RTLD_NEXT, "closedir");
    real_readdir = dlsym(RTLD_NEXT, "readdir");
    real_readdir64 = dlsym(RTLD_NEXT, "readdir64");
    real_readdir_r = dlsym(RTLD_NEXT, "readdir_r");
    real_readdir64_r = dlsym(RTLD_NEXT, "readdir64_r");
    real_telldir = dlsym(RTLD_NEXT, "telldir");
    real_seekdir = dlsym(RTLD_NEXT, "seekdir");
    real_dirfd = dlsym(RTLD_NEXT, "dirfd");
    if ((cp = safe_getenv("SPD_READDIR_MAX_SIZE")) != NULL)
    {
        max_dirsize = atol(cp);
    }
#ifdef DEBUG
    if (safe_getenv("SPD_READDIR_DEBUG"))
    {
        do_debug++;
    }
#endif
}

static void free_cached_dir(struct dir_s *dirstruct)
{
    int i;

    pthread_mutex_destroy(&(dirstruct->lock));

    if (!dirstruct->dp)
    {
        return;
    }

    for (i = 0; i < dirstruct->num; i++)
    {
        free(dirstruct->dp[i].d_name);
    }
    free(dirstruct->dp);
    dirstruct->dp = 0;
}

static int ino_cmp(const void *a, const void *b)
{
    const struct dirent_s *ds_a = (const struct dirent_s *) a;
    const struct dirent_s *ds_b = (const struct dirent_s *) b;
    ino_t i_a, i_b;

    i_a = ds_a->d_ino;
    i_b = ds_b->d_ino;

    if (ds_a->d_name[0] == '.')
    {
        if (ds_a->d_name[1] == 0)
        {
            i_a = 0;
        }
        else if ((ds_a->d_name[1] == '.') && (ds_a->d_name[2] == 0))
        {
            i_a = 1;
        }
    }
    if (ds_b->d_name[0] == '.')
    {
        if (ds_b->d_name[1] == 0)
        {
            i_b = 0;
        }
        else if ((ds_b->d_name[1] == '.') && (ds_b->d_name[2] == 0))
        {
            i_b = 1;
        }
    }

    return (i_a - i_b);
}

static struct dir_s *open_directory(DIR *dir, struct stat *st)
{
    struct dir_s    *dirstruct;
    struct dirent_s *ds, *dnew;
    struct dirent64 *d;
    static pthread_mutexattr_t mutexattr;
    mutexattr.__align = PTHREAD_MUTEX_RECURSIVE;

    dirstruct = malloc(sizeof(struct dir_s));
    if (!dirstruct)
    {
        (*real_closedir)(dir);
        errno = -ENOMEM;
        return NULL;
    }
    pthread_mutex_init(&(dirstruct->lock), &mutexattr);
    dirstruct->num = 0;
    dirstruct->max = 0;
    dirstruct->dp = 0;
    dirstruct->pos = 0;
    dirstruct->dir = 0;
    dirstruct->fd = -1;

    if (max_dirsize && (st->st_size > max_dirsize))
    {
        DEBUG_DIR(printf("Directory size %d, using direct readdir\n",
                         (int)st->st_size));
        dirstruct->dir = dir;
        return dirstruct;
    }

    while ((d = (*real_readdir64)(dir)) != NULL)
    {
        if (dirstruct->num >= dirstruct->max)
        {
            dirstruct->max += ALLOC_STEPSIZE;
            DEBUG_DIR(printf("Reallocating to size %d\n",
                             dirstruct->max));
            dnew = realloc(dirstruct->dp,
                           dirstruct->max * sizeof(struct dirent_s));
            if (!dnew)
            {
                goto nomem;
            }
            dirstruct->dp = dnew;
        }
        ds = &dirstruct->dp[dirstruct->num++];
        ds->d_ino = d->d_ino;
        ds->d_off = d->d_off;
        ds->d_reclen = d->d_reclen;
        ds->d_type = d->d_type;
        if ((ds->d_name = malloc(strlen(d->d_name) + 1)) == NULL)
        {
            dirstruct->num--;
            goto nomem;
        }
        strcpy(ds->d_name, d->d_name);
        DEBUG_DIR(printf("readdir: %lu %s\n",
                         (unsigned long) d->d_ino, d->d_name));
    }
    dirstruct->fd = dup((*real_dirfd)(dir));
    (*real_closedir)(dir);
    qsort(dirstruct->dp, dirstruct->num, sizeof(struct dirent_s), ino_cmp);
#if 0
    if (do_debug)
    {
        int i;
        printf("After sorting.\n");
        for (i = 0; i < dirstruct->num; i++)
            printf("%lu %s\n",
                   (unsigned long) dirstruct->dp[i].d_ino, dirstruct->dp[i].d_name);
    }
#endif
    return dirstruct;
nomem:
    DEBUG_DIR(printf("No memory, backing off to direct readdir\n"));
    free_cached_dir(dirstruct);
    dirstruct->dir = dir;
    return dirstruct;
}

DIR *opendir(const char *name)
{
    DIR *dir;
    struct stat st;

    memset(&st, 0, sizeof(struct stat));
    if (!real_opendir)
    {
        setup_ptr();
    }

    DEBUG_DIR(printf("Opendir(%s) (%d open)\n", name, num_open++));
    dir = (*real_opendir)(name);
    if (!dir)
    {
        return NULL;
    }

    if (max_dirsize && stat(name, &st) < 0)
    {
        st.st_size = max_dirsize + 1;
    }

    return (DIR *) open_directory(dir, &st);
}

DIR *fdopendir(int fd)
{
    DIR *dir;
    struct stat st;

    memset(&st, 0, sizeof(struct stat));
    if (!real_fdopendir)
    {
        setup_ptr();
    }

    DEBUG_DIR(printf("fdopendir(%d) (%d open)\n", fd, num_open++));
    if (max_dirsize && fstat(fd, &st) < 0)
    {
        st.st_size = max_dirsize + 1;
    }

    dir = (*real_fdopendir)(fd);
    if (!dir)
    {
        return NULL;
    }

    return (DIR *) open_directory(dir, &st);
}

int closedir(DIR *dir)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;

    DEBUG_DIR(printf("Closedir (%d open)\n", --num_open));
    if (dirstruct->dir)
    {
        (*real_closedir)(dirstruct->dir);
    }

    if (dirstruct->fd >= 0)
    {
        close(dirstruct->fd);
    }
    free_cached_dir(dirstruct);
    free(dirstruct);
    return 0;
}

struct dirent *readdir(DIR *dir)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;
    struct dirent_s *ds;

    if (dirstruct->dir)
    {
        return (*real_readdir)(dirstruct->dir);
    }

    if (dirstruct->pos >= dirstruct->num)
    {
        return NULL;
    }

    ds = &dirstruct->dp[dirstruct->pos++];
    dirstruct->ret_dir.d_ino = ds->d_ino;
    dirstruct->ret_dir.d_off = ds->d_off;
    dirstruct->ret_dir.d_reclen = ds->d_reclen;
    dirstruct->ret_dir.d_type = ds->d_type;
    strncpy(dirstruct->ret_dir.d_name, ds->d_name,
            sizeof(dirstruct->ret_dir.d_name));

    return (&dirstruct->ret_dir);
}

int readdir_r(DIR *__restrict dir,
              struct dirent *__restrict entry,
              struct dirent **__restrict result)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;
    struct dirent_s *ds;

    if (dirstruct->dir)
    {
        return (*real_readdir_r)(dir, entry, result);
    }
    pthread_mutex_lock(&(dirstruct->lock));
    if (dirstruct->pos >= dirstruct->num)
    {
        *result = NULL;
    }
    else
    {
        ds = &dirstruct->dp[dirstruct->pos++];
        entry->d_ino = ds->d_ino;
        entry->d_off = ds->d_off;
        entry->d_reclen = ds->d_reclen;
        entry->d_type = ds->d_type;
        strncpy(entry->d_name, ds->d_name,
                sizeof(entry->d_name));
        *result = entry;
    }
    pthread_mutex_unlock(&(dirstruct->lock));
    return 0;
}

struct dirent64 *readdir64(DIR *dir)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;
    struct dirent_s *ds;

    if (dirstruct->dir)
    {
        return (*real_readdir64)(dirstruct->dir);
    }

    if (dirstruct->pos >= dirstruct->num)
    {
        return NULL;
    }

    ds = &dirstruct->dp[dirstruct->pos++];
    dirstruct->ret_dir64.d_ino = ds->d_ino;
    dirstruct->ret_dir64.d_off = ds->d_off;
    dirstruct->ret_dir64.d_reclen = ds->d_reclen;
    dirstruct->ret_dir64.d_type = ds->d_type;
    strncpy(dirstruct->ret_dir64.d_name, ds->d_name,
            sizeof(dirstruct->ret_dir64.d_name));

    return (&dirstruct->ret_dir64);
}

int readdir64_r(DIR *__restrict dir,
                struct dirent64 *__restrict entry,
                struct dirent64 **__restrict result)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;
    struct dirent_s *ds;

    if (dirstruct->dir)
    {
        return (*real_readdir64_r)(dir, entry, result);
    }
    pthread_mutex_lock(&(dirstruct->lock));
    if (dirstruct->pos >= dirstruct->num)
    {
        *result = NULL;
    }
    else
    {
        ds = &dirstruct->dp[dirstruct->pos++];
        entry->d_ino = ds->d_ino;
        entry->d_off = ds->d_off;
        entry->d_reclen = ds->d_reclen;
        entry->d_type = ds->d_type;
        strncpy(entry->d_name, ds->d_name,
                sizeof(entry->d_name));
        *result = entry;
    }
    pthread_mutex_unlock(&(dirstruct->lock));
    return 0;
}

off_t telldir(DIR *dir)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;

    if (dirstruct->dir)
    {
        return (*real_telldir)(dirstruct->dir);
    }

    return ((off_t) dirstruct->pos);
}

void seekdir(DIR *dir, off_t offset)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;

    if (dirstruct->dir)
    {
        (*real_seekdir)(dirstruct->dir, offset);
        return;
    }

    dirstruct->pos = offset;
}

int dirfd(DIR *dir)
{
    struct dir_s    *dirstruct = (struct dir_s *) dir;

    if (dirstruct->dir)
    {
        return (*real_dirfd)(dirstruct->dir);
    }

    return (dirstruct->fd);
}
