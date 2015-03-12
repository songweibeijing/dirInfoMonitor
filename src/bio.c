#include "header.h"
#include "bio.h"
#include "log.h"
#include "linux_list.h"
#include "atomic.h"

#define THREAD_STACK_SIZE (1024*1024*4)

int g_build_index_ok = 0;
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];
static pthread_cond_t bio_condvar[BIO_NUM_OPS];
static pthread_cond_t bio_condvar_empty[BIO_NUM_OPS];
struct list_head bio_jobs[BIO_NUM_OPS];
static unsigned long long bio_pending[BIO_NUM_OPS];
static unsigned int g_max_bio_penging = 1024000;
static unsigned int g_max_free_bio_obj = 1024000;
static unsigned int g_free_objs = 0;

struct bio_job
{
    bio_handle bh;
    void *arg;
    struct list_head list;
};
struct list_head free_bio_job_list;
static pthread_mutex_t free_bio_job_mutex;

struct bio_job *new_bio_job()
{
    struct bio_job *job = NULL;

    pthread_mutex_lock(&free_bio_job_mutex);
    if (!list_empty(&free_bio_job_list))
    {
        job = list_entry(free_bio_job_list.next, struct bio_job, list);
        list_del(&job->list);
    }
    pthread_mutex_unlock(&free_bio_job_mutex);

    if (job == NULL)
    {
        job = calloc(1, sizeof(*job));
        if (job != NULL)
        {
            pthread_mutex_lock(&free_bio_job_mutex);
            g_free_objs++;
            pthread_mutex_unlock(&free_bio_job_mutex);
        }
    }

    return job;
}

void free_bio_job(struct bio_job *jobs)
{
    if (jobs == NULL)
    {
        return;
    }

    int freeit = 0;

    pthread_mutex_lock(&free_bio_job_mutex);
    if (g_free_objs >= g_max_free_bio_obj)
    {
        freeit = 1;
        g_free_objs--;
    }
    else
    {
        list_add_tail(&jobs->list, &free_bio_job_list);
    }
    pthread_mutex_unlock(&free_bio_job_mutex);

    if (freeit)
    {
        my_free(jobs);
    }
}

void *bio_process_jobs(void *arg);

void mysleep(int sec)
{
    struct timeval interval;
    interval.tv_sec = sec;
    interval.tv_usec = 0;
    select(0, NULL, NULL, NULL, &interval);
}

/* Initialize the background system, spawning the thread. */
int bio_init(void)
{
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize = 0;
    int j = 0;

    INIT_LIST_HEAD(&free_bio_job_list);
    pthread_mutex_init(&free_bio_job_mutex, NULL);
    /* Initialization of state vars and objects */
    for (j = 0; j < BIO_NUM_OPS; j++)
    {
        pthread_mutex_init(&bio_mutex[j], NULL);
        pthread_cond_init(&bio_condvar[j], NULL);
        pthread_cond_init(&bio_condvar_empty[j], NULL);
        INIT_LIST_HEAD(&bio_jobs[j]);
        bio_pending[j] = 0;
    }

    /* Set the stack size as by default it may be small in some system */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stacksize);
    if (!stacksize)
    {
        stacksize = 1;    /* The world is full of Solaris Fixes */
    }
    while (stacksize < THREAD_STACK_SIZE)
    {
        stacksize *= 2;
    }
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass the job ID the thread is
     * responsible of. */
    for (j = 0; j < BIO_NUM_OPS; j++)
    {
        void *arg = (void *)(unsigned long) j;
        if (pthread_create(&thread, &attr, bio_process_jobs, arg) != 0)
        {
            printf("Fatal: Can't initialize Background Jobs.\n");
            return -1;
        }
    }
    return 0;
}

int g_show_logs = 0;

void bio_create_job(int type, bio_handle bh, void *arg)
{
    int bio_num = 0;
    struct bio_job *job = new_bio_job();
    if (job == NULL)
    {
        return;
    }

    job->bh = bh;
    job->arg = arg;

    INIT_LIST_HEAD(&job->list);
    pthread_mutex_lock(&bio_mutex[type]);
    list_add_tail(&job->list, &bio_jobs[type]);
    bio_num = ++bio_pending[type];
    pthread_cond_signal(&bio_condvar[type]);
    pthread_mutex_unlock(&bio_mutex[type]);

    if (bio_num > g_max_bio_penging)
    {
        if (g_show_logs % 1000 == 0)
        {
            debug_sys(LOG_NOTICE, "There are so many jobs in queue\n");
            g_show_logs = 0;
        }
        g_show_logs++;
    }
}

void *bio_process_jobs(void *arg)
{
    struct bio_job *ln = NULL;
    unsigned long type = (unsigned long) arg;

    pthread_detach(pthread_self());

    //wait the initial process is ready.
    while (g_build_index_ok == 0)
    {
        //if the start flag is 0, sleep 5s, and try it again.
        mysleep(1);
    }

    pthread_mutex_lock(&bio_mutex[type]);
    while (1)
    {
        /* The loop always starts with the lock hold. */
        if (list_empty(&bio_jobs[type]))
        {
            pthread_cond_wait(&bio_condvar[type], &bio_mutex[type]);
            continue;
        }
        /* Pop the job from the queue. */
        ln = list_entry(bio_jobs[type].next, struct bio_job, list);
        pthread_mutex_unlock(&bio_mutex[type]);

        //process it here
        ln->bh(ln->arg);

        pthread_mutex_lock(&bio_mutex[type]);
        list_del(&ln->list);
        free_bio_job(ln);
        bio_pending[type]--;
        if (bio_pending[type] == 0)
        {
            pthread_cond_signal(&bio_condvar_empty[type]);
        }
    }
}

/* Return the number of pending jobs of the specified type. */
unsigned long long bio_jobnum(int type)
{
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

static void wait_for_one_thread(int type)
{
    pthread_mutex_lock(&bio_mutex[type]);
    while (1)
    {
        if (bio_pending[type] == 0)
        {
            break;
        }

        if (bio_pending[type] > 0)
        {
            pthread_cond_wait(&bio_condvar_empty[type], &bio_mutex[type]);
            continue;
        }
    }
    pthread_mutex_unlock(&bio_mutex[type]);
}

//wait until all bio worker threads to finish their jobs.
//bio worker index starts from HANDLE_INOTIFY_THREADED to BIO_NUM_OPS.
void wait_for_bio_threads()
{
    int i = HANDLE_INOTIFY_THREADED;
    for (; i < BIO_NUM_OPS; i++)
    {
        wait_for_one_thread(i);
    }
}


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

#define LOCK_DIV    10
#define OBJECT_HASH 1024000
static pthread_mutex_t object_mutex[OBJECT_HASH / LOCK_DIV];
static pthread_mutex_t swap_mutex;
static struct list_head g_swap_head;
static volatile int swap_backet_num = -1;
static struct list_head object_hash[OBJECT_HASH];
static int object_num = 0;

//max memory is 2G * division
#define division 4
#define half_div (division/2)
atomic_t mem_usage;

static int int_conv(size_t size)
{
    int mod = size % division;
    int div = size / division;
    if (mod >= half_div)
    {
        return div + 1;
    }
    return div;
}

void add_mem(size_t size)
{
    int conv = int_conv(size);
    atomic_add(conv, &mem_usage);
}

void sub_mem(size_t size)
{
    int conv = int_conv(size);
    atomic_sub(conv, &mem_usage);
}

uint64_t get_mem()
{
    uint64_t size = (uint64_t)atomic_read(&mem_usage);
    uint64_t div64 = (uint64_t)division;
    uint64_t realsize = size * div64;
    return realsize;
}

static int get_mem_obj_size(char *path)
{
    return strlen(path) + sizeof(mem_obj) + 1;
}

mem_obj *alloc_mem_obj(char *path, fileinfo *fi)
{
    int size = get_mem_obj_size(path);
    mem_obj *obj = (mem_obj *) calloc(1, size);
    if (obj == NULL)
    {
        return NULL;
    }
    INIT_LIST_HEAD(&obj->hash);
    memcpy(&obj->fi, fi, sizeof(*fi));
    strncpy(obj->path, path, strlen(path));
    return obj;
}

void free_mem_obj(mem_obj *obj)
{
    my_free(obj);
}

static unsigned int simple_hash(char *str)
{
    unsigned int hash = 0;
    unsigned char *p = NULL;

    for (hash = 0, p = (unsigned char *)str; *p ; p++)
    {
        hash = 31 * hash + *p;
    }

    return (hash & 0x7FFFFFFF) % OBJECT_HASH;
}

static int compare_key(char *key1, char *key2)
{
    int sz = strlen(key1);
    if (sz != strlen(key2))
    {
        return -1;
    }
    if (strncmp(key1, key2, sz) != 0)
    {
        return -1;
    }
    return 0;
}

//function without lock
static void *__get_object_cache(char *key, int hash)
{
    struct list_head *list;
    mem_obj *obj = NULL, *ret = NULL;

    list_for_each(list, &object_hash[hash])
    {
        obj = list_entry(list, mem_obj, hash);
        if (compare_key(obj->path, key) == 0)
        {
            ret = obj;
            break;
        }
    }

    if (swap_backet_num == hash)
    {
        pthread_mutex_lock(&swap_mutex);
        list_for_each(list, &g_swap_head)
        {
            obj = list_entry(list, mem_obj, hash);
            if (compare_key(obj->path, key) == 0)
            {
                ret = obj;
                break;
            }
        }
        pthread_mutex_unlock(&swap_mutex);
    }

    return ret;
}

int get_object_cache_value(char *key, fileinfo *fi)
{
    int found = 0;
    int hash = simple_hash(key);
    int lock_hash = hash / LOCK_DIV;
    mem_obj *ret = NULL;

    pthread_mutex_lock(&object_mutex[lock_hash]);
    ret = __get_object_cache(key, hash);
    if (ret)
    {
        found = FOUND;
        memcpy(fi, &ret->fi, sizeof(fileinfo));
    }
    pthread_mutex_unlock(&object_mutex[lock_hash]);
    return found;
}

void *add_object_cache(char *key, fileinfo *fi)
{
    int hash = simple_hash(key);
    int lock_hash = hash / LOCK_DIV;
    mem_obj *obj = NULL, *ret = NULL;

    pthread_mutex_lock(&object_mutex[lock_hash]);
    ret = __get_object_cache(key, hash);
    if (ret != NULL)
    {
        memcpy(&ret->fi, fi, sizeof(*fi));
        goto out;
    }

    obj = alloc_mem_obj(key, fi);
    if (!obj)
    {
        goto out;
    }

    list_add(&obj->hash, &object_hash[hash]);
    object_num++;
    add_mem(get_mem_obj_size(key));
    ret = obj;

out:
    pthread_mutex_unlock(&object_mutex[lock_hash]);
    return ret;
}

int delete_object_cache(char *key)
{
    int hash = simple_hash(key);
    int lock_hash = hash / LOCK_DIV;
    mem_obj *ret = NULL;

    pthread_mutex_lock(&object_mutex[lock_hash]);
    ret = __get_object_cache(key, hash);
    if (ret != NULL)
    {
        list_del(&ret->hash);
        object_num--;
        sub_mem(get_mem_obj_size(key));
    }
    pthread_mutex_unlock(&object_mutex[lock_hash]);

    if (ret != NULL)
    {
        free_mem_obj(ret);
        return FOUND;
    }
    return NFOUND;
}

//it must be called with object_mutex
static int create_swap_list(int index)
{
    pthread_mutex_lock(&swap_mutex);
    list_splice_init(&object_hash[index], &g_swap_head);
    swap_backet_num = index;
    pthread_mutex_unlock(&swap_mutex);
    return 0;
}

//this function is not thread_safe, it is only used in one thread.
int get_swap_list()
{
    static int last_swap_bucket = -1;
    int i, found, bucket, lock_hash, retry = 5;
    i = 0;

    while (i < retry)
    {
        found = 0;
        bucket  = rand() % OBJECT_HASH;
        lock_hash = bucket / LOCK_DIV;

        pthread_mutex_lock(&object_mutex[lock_hash]);
        if (!list_empty(&object_hash[bucket]))
        {
            create_swap_list(bucket);
            found = 1;
        }
        pthread_mutex_unlock(&object_mutex[lock_hash]);

        if (found == 1)
        {
            break;
        }

        i++;
    }

    if (found == 0)
    {
        i = (last_swap_bucket + 1) % OBJECT_HASH;
        //try to find it from the begin
        for (; i < OBJECT_HASH && found != 1; i++)
        {
            pthread_mutex_lock(&object_mutex[lock_hash]);
            if (!list_empty(&object_hash[i]))
            {
                last_swap_bucket = i;
                found = 1;
                create_swap_list(i);
            }
            pthread_mutex_unlock(&object_mutex[lock_hash]);
        }
    }

    return found;
}

int swap_mem_2_db(swap_func func, void *arg1)
{
    struct list_head *list = NULL, *list1 = NULL;
    mem_obj *obj = NULL;
    debug_sys(LOG_DEBUG, "begin to call get_swap_list\n");

    if (get_swap_list() == 1)
    {
        debug_sys(LOG_DEBUG, "found swap list\n");
        pthread_mutex_lock(&swap_mutex);
        list_for_each_safe(list, list1, &g_swap_head)
        {
            obj = list_entry(list, mem_obj, hash);
            func(arg1, (void *)obj);

            list_del(&obj->hash);
            object_num--;
            sub_mem(get_mem_obj_size(obj->path));
            free_mem_obj(obj);
        }
        swap_backet_num = -1;
        pthread_mutex_unlock(&swap_mutex);
    }

    return 0;
}

int mem_object_init()
{
    int i;
    atomic_set(&mem_usage, 0);
    for (i = 0; i < OBJECT_HASH; i++)
    {
        if (i % LOCK_DIV == 0)
        {
            pthread_mutex_init(&object_mutex[i / LOCK_DIV], NULL);
        }
        INIT_LIST_HEAD(&object_hash[i]);
    }

    pthread_mutex_init(&swap_mutex, NULL);
    INIT_LIST_HEAD(&g_swap_head);
    swap_backet_num = -1;

    return 0;
}

