#include "header.h"
#include "headercxx.h"
#include "inotify_process.h"
#include "monitor_dir.h"
#include "kv.h"
#include "util.h"
#include "log.h"
#include "config.h"
#include "dump.h"
#include "shm_api.h"

extern config g_config;
extern vector<action_item *> *action_list;
extern bdb_info *g_hash_db;
extern int g_build_index_ok;
int g_first_dump = 0;

static shm_handle_t index_handle;
pthread_rwlock_t g_action_lock = PTHREAD_RWLOCK_INITIALIZER;
rc_index *p_indexs = NULL;

int create_shm_index()
{
    void *pindex = NULL;

    unsigned int i = 0;
    struct shm_attr attr = {0};
    attr.entry_size = sizeof(rc_index);
    attr.n_entry = MAX_INDEX;

    p_indexs = (rc_index *)calloc(MAX_INDEX, sizeof(rc_index));
    if (p_indexs == NULL)
    {
        return -1;
    }

    index_handle = shm_attach((char *)SHM_DIRCOUNTER_INDEX);
    if (index_handle == SHM_HANDLE_FAIL)
    {
        index_handle = shm_create((char *)SHM_DIRCOUNTER_INDEX, &attr);
        if (index_handle == NULL)
        {
            free(p_indexs);
            return -1;
        }
    }

    shm_wlock(index_handle);
    for_each_shm_obj(index_handle, pindex, i)
    {
        memset(pindex, 0, sizeof(rc_index));
    }
    shm_wunlock(index_handle);
    return 0;
}

static unsigned int RSHash(char *src, unsigned int length)
{
    unsigned int b    = 378551;
    unsigned int a    = 63689;
    unsigned int hash = 0;

    for (std::size_t i = 0; i < length; i++)
    {
        hash = hash * a + src[i];
        a    = a * b;
    }

    return hash % (MAX_INDEX - 1);
}

static int __update_index(vector<txn_param> &params_dirs, int fd, int *p_rev_rank)
{
    void *pindex = NULL;
    int ret = 0, hval = 0, k = 0;
    int tm = time(NULL);
    unsigned int i = 0;
    data_rec rec;
    unsigned int size = params_dirs.size();

    memset(p_indexs, 0, sizeof(rc_index) * MAX_INDEX);
    for (vector<txn_param>::iterator it = params_dirs.begin();
         it != params_dirs.end(); it++)
    {
        hval = RSHash((char *)it->key, it->keysize);
        p_indexs[hval].cnt++;
        p_indexs[hval].in_use = 1;
    }

    //restore the index value.
    for (i = 1; i < MAX_INDEX - 1; i++)
    {
        p_indexs[i].index = p_indexs[i - 1].index + p_indexs[i - 1].cnt;
    }

    //find the rank for each item
    i = 0;
    for (vector<txn_param>::iterator it = params_dirs.begin();
         it != params_dirs.end(); it++)
    {
        hval = RSHash((char *)it->key, it->keysize);
        p_rev_rank[p_indexs[hval].index++] = i++;
    }

    for (i = 0; i < MAX_INDEX - 1; i++)
    {
        p_indexs[i].index = p_indexs[i].index - p_indexs[i].cnt;
    }

    for (i = 0; i < size; i++)
    {
        txn_param *p = &params_dirs[p_rev_rank[i]];
        memset(&rec, 0, sizeof(data_rec));
        if (p->keysize >= 256 || p->valuesize != sizeof(fileinfo))
        {
            k++;
            continue;
        }

        memcpy(rec.file, p->key, p->keysize);
        memcpy(&rec.fi, p->value, p->valuesize);

        ret = write(fd, &rec, sizeof(data_rec));
        if (ret == -1)
        {
            debug_sys(LOG_ERR, "Call write failed for tmp data file, error %s\n", strerror(errno));
            return -1;
        }
    }

    shm_wlock(index_handle);
    ret = rename(DATA_FILE_TMP, DATA_FILE);
    if (ret == -1)
    {
        debug_sys(LOG_ERR, "Call rename failed for tmp data file, error %s\n", strerror(errno));
    }
    if (ret == 0)
    {
        //record the time here
        p_indexs[MAX_INDEX - 1].index = tm;
        p_indexs[MAX_INDEX - 1].in_use = 1;
        for_each_shm_obj(index_handle, pindex, i)
        {
            memcpy(pindex, p_indexs + i, sizeof(rc_index));
        }
    }
    shm_wunlock(index_handle);
    return ret;
}

static int update_index()
{
    int fd = 0, ret = -1;
    int *p_rev_rank = NULL;
    vector<txn_param> params_dirs;
    params_dirs.clear();
    convert_to_param(g_md, params_dirs);

    unlink(DATA_FILE_TMP);
    fd = open(DATA_FILE_TMP, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
    {
        debug_sys(LOG_ERR, "Failed to open file %s, error %s\n", DATA_FILE_TMP, strerror(errno));
        goto failed;
    }
    debug_sys(LOG_DEBUG, "PARAM DIR ENTRY SIZE %d\n", params_dirs.size());
    ftruncate(fd, params_dirs.size()*sizeof(data_rec));

    p_rev_rank = (int *)calloc(params_dirs.size(), sizeof(int));
    if (p_rev_rank == NULL)
    {
        goto failed;
    }

    ret = __update_index(params_dirs, fd, p_rev_rank);

failed:
    if (fd > 0)
    {
        close(fd);
    }
    my_free(p_rev_rank);
    return ret;
}

data_rec test_one_key(char *key, int length)
{
    data_rec dr;
    memset(&dr, 0, sizeof(data_rec));

    record_index ri;
    memset(&ri, 0, sizeof(record_index));

    int hval = RSHash(key, length);
    shm_rlock(index_handle);
    void *data = shm_obj_ptr(index_handle, hval);
    memcpy(&ri, data, sizeof(record_index));
    shm_runlock(index_handle);

    if (ri.cnt == 0 || ri.in_use == 0)
    {
        return dr;
    }

    int fd = open(DATA_FILE, O_RDONLY);
    if (fd < 0)
    {
        debug_sys(LOG_ERR, "Failed to open file %s, error %s\n", DATA_FILE, strerror(errno));
        return dr;
    }

    char *data_array = (char *)calloc(ri.cnt, sizeof(data_rec));
    if (data_array == NULL)
    {
        close(fd);
        return dr;
    }

    lseek(fd, sizeof(data_rec) * ri.index, SEEK_SET);
    read(fd, data_array, sizeof(data_rec) * ri.cnt);
    close(fd);

    data_rec *pdata = (data_rec *)data_array;
    for (unsigned int i = 0; i < ri.cnt; i++)
    {
        if (strcmp(pdata->file, key) == 0)
        {
            dr = *pdata;
            free(data_array);
            return dr;
        }
        pdata++;
    }
    free(data_array);
    return dr;
}

void print_test_result(char *key, data_rec *pdata)
{
    debug_sys(LOG_DEBUG, "key %s, number %lld, size %lld\n", key, pdata->fi.filenm, pdata->fi.filesz);
}

void *do_dump(void *arg)
{
    pthread_detach(pthread_self());

    while (1)
    {
        //if the start flag is 0, sleep 5s, and try it again.
        my_sleep(g_config.dump_interval);
        if (g_build_index_ok == 0)
        {
            continue;
        }
        //dump to index file and data file.
        update_index();
    }

    return NULL;
}

int dump_thread_create()
{
    pthread_t thread_id;

    int ret = create_shm_index();
    if (ret != 0)
    {
        debug_sys(LOG_ERR, "Call create_shm_index failed\n");
        return -1;
    }

    if (pthread_create(&thread_id, NULL, do_dump, NULL))
    {
        return -1;
    }

    return 0;
}
