#ifndef _BIO_H
#define _BIO_H

#include "header.h"
#include "linux_list.h"

#if defined(__cplusplus)
extern "C" {
#endif

    /* Background job opcodes */
#define POSTED_HANDLE               0
#define HANDLE_INOTIFY              1
#define HANDLE_INOTIFY_THREADED     2
#define BIO_NUM_OPS                 130

    typedef int (*bio_handle)(void *arg);

    extern int g_build_index_ok;

    /* Exported API */
    int bio_init(void);
    void bio_create_job(int type, bio_handle bh, void *arg);
    unsigned long long bio_jobnum(int type);
    void wait_for_bio_threads();

    void add_mem(size_t size);
    void sub_mem(size_t size);
    uint64_t get_mem();

    typedef struct mem_obj
    {
        struct list_head hash;
        fileinfo fi;
        char path[0];
    } mem_obj;

    typedef int (*swap_func)(void *arg1, void *arg2);

    mem_obj *alloc_mem_obj(char *path, fileinfo *fi);
    void free_mem_obj(mem_obj *obj);
#if 0
    void *get_object_cache(char *key);
#endif
    int get_object_cache_value(char *key, fileinfo *fi);
    void *add_object_cache(char *key, fileinfo *fi);
    int delete_object_cache(char *key);
    int get_swap_list();

    int swap_mem_2_db(swap_func func, void *arg1);
    int mem_object_init(void);


#if defined(__cplusplus)
}
#endif

#endif
