#include "header.h"
#include "headercxx.h"
#include "util.h"
#include "kv.h"
#include "bio.h"
#include "config.h"
#include "log.h"

extern config g_config;
extern int g_build_index_ok;

static DB_ENV *db_init(char *home);
static void *sync_kv_process(void *arg);
static void *swap_kv_process(void *arg);
static void *deadlock(void *arg);
static void *logfile_thread(void *arg);

bdb_info *init_kv_storage(char *dir, char *dbname, enum BDB_TYPE type, int rm, int swap)
{
    int ret = 0;
    pthread_t tid;
    DB_TXN *txnp = NULL;
    DB     *dbp = NULL;                 /* Database handle. */
    DB_ENV *dbenv = NULL;               /* Database environment. */
    DBTYPE dbtype =  DB_HASH;
    bdb_info *db = NULL;

    if (type == HASH)
    {
        dbtype = DB_HASH;
    }
    else if (type == BTREE)
    {
        dbtype = DB_BTREE;
    }

    //remove the old db first
    if (rm == 1)
    {
        char rmcmd[1024] = {0};
        snprintf(rmcmd, 1024, "rm -rf %s", dir);
        system(rmcmd);
    }

    make_dir_recusive(dir, 0x777);

    /* Initialize the database environment. */
    dbenv = db_init(dir);
    if (dbenv == NULL)
    {
        debug_sys(LOG_ERR, "call db_init to create dbenv failed\n");
        return NULL;
    }

    debug_sys(LOG_NOTICE, "begin to create db %s\n", dir);
    /* Initialize the database. */
    if ((ret = db_create(&dbp, dbenv, 0)) != 0)
    {
        debug_sys(LOG_ERR, "db_create for %s failed, ret : %d, %s \n", dir, ret, db_strerror(ret));
        dbenv->err(dbenv, ret, "db_create");
        (void)dbenv->close(dbenv, 0);
        return NULL;
    }
    debug_sys(LOG_NOTICE, "db_create ok for db %s\n", dir);

    if ((ret = dbp->set_pagesize(dbp, 1024)) != 0)
    {
        debug_sys(LOG_ERR, "call set_pagesize failed, ret %d\n", ret);
        dbp->err(dbp, ret, "set_pagesize");
        goto err;
    }

    if ((ret = dbenv->txn_begin(dbenv, NULL, &txnp, 0)) != 0)
    {
        debug_sys(LOG_ERR, "call txn_begin :%d\n", ret);
        return NULL;
    }

    if ((ret = dbp->open(dbp, txnp, (char *)dbname, NULL, dbtype, DB_CREATE | DB_THREAD, 0664)) != 0)
    {
        debug_sys(LOG_ERR, "call open :%d\n", ret);
        dbp->err(dbp, ret, "%s: open", dbname);
        goto err;
    }

    ret = txnp->commit(txnp, 0);
    txnp = NULL;
    if (ret != 0)
    {
        debug_sys(LOG_ERR, "call commit failed, ret %d\n", ret);
        goto err;
    }

    db = (bdb_info *)calloc(1, sizeof(bdb_info));
    if (db == NULL)
    {
        debug_sys(LOG_ERR, "malloc failed\n");
        goto err;
    }
    snprintf(db->dbdir, MAX_PATH, "%s", dir);
    snprintf(db->dbname, MAX_PATH, "%s", dbname);
    db->type = type;
    db->dbp = dbp;
    db->dbenv  = dbenv;
    pthread_rwlock_init(&db->db_lock, NULL);

    if (swap == 1)
    {
        if ((ret = pthread_create(&tid, NULL, swap_kv_process, db)) != 0)
        {
            debug_sys(LOG_ERR, "call pthread_create error:%d\n", errno);
        }
    }
    if ((ret = pthread_create(&tid, NULL, sync_kv_process, db)) != 0)
    {
        debug_sys(LOG_ERR, "call pthread_create error:%d\n", errno);
    }
    if ((ret = pthread_create(&tid, NULL, deadlock, db)) != 0)
    {
        debug_sys(LOG_ERR, "call pthread_create error:%d\n", errno);
    }

    if ((ret = pthread_create(&tid, NULL, logfile_thread, db)) != 0)
    {
        debug_sys(LOG_ERR, "call pthread_create error:%d\n", errno);
    }

    debug_sys(LOG_NOTICE, "init kv storage successfully\n");
    return db;

err:
    debug_sys(LOG_ERR, "init kv storage failed\n");
    if (txnp != NULL)
    {
        (void)txnp->abort(txnp);
    }
    (void)dbp->close(dbp, 0);
    (void)dbenv->close(dbenv, 0);
    my_free(db);
    return NULL;
}

int deinit_kv_storage(bdb_info *db)
{
    pthread_rwlock_destroy(&db->db_lock);
    (void)db->dbp->close(db->dbp, 0);
    (void)db->dbenv->close(db->dbenv, 0);
    return 0;
}

int insert_key_value(bdb_info *db, char *buf, fileinfo value)
{
    return insert_key_value_basic(db, buf, strlen(buf), &value, sizeof(value), NULL);
}

int insert_key_value_cache(bdb_info *db, char *buf, fileinfo value)
{
    if (get_mem() < g_config.max_memory)
    {
        void *obj = add_object_cache(buf, &value);
        if (obj == NULL)
        {
            debug_sys(LOG_ERR, "error to call add_object_cache\n");
        }
        else
        {
            return 0;
        }
    }

    return insert_key_value(db, buf, value);
}

int get_key_value(bdb_info *db, char *buf, fileinfo *value)
{
    return get_key_value_basic(db, buf, strlen(buf), value, sizeof(*value), NULL);
}

int get_key_value_cache(bdb_info *db, char *buf, fileinfo *value)
{
    if (get_object_cache_value(buf, value) == FOUND)
    {
        return FOUND;
    }
    return get_key_value(db, buf, value);
}

int delete_key(bdb_info *db, char *buf)
{
    return delete_key_basic(db, buf, strlen(buf), NULL);
}

int delete_key_cache(bdb_info *db, char *buf)
{
    int ret = delete_object_cache(buf);
    if (ret == FOUND)
    {
        return 0;
    }
    return delete_key(db, buf);
}

int process_db_batch(bdb_info *db, vector<txn_param> params)
{
    int ret = 0;
    for (vector<txn_param>::iterator it = params.begin();
         it != params.end(); it++)
    {
        txn_param tp = *it;
        switch (tp.type)
        {
            case INSERT:
                ret = insert_key_value_basic(db, tp.key, tp.keysize, tp.value, tp.valuesize, NULL);
                if (ret != 0)
                {
                    debug_sys(LOG_ERR, "failed to call put for %s, ret :%d\n", (char *)tp.key, ret);
                    continue;
                }
                break;
            case DELETE:
                ret = delete_key_basic(db, tp.key, tp.keysize, NULL);
                if (ret != 0)
                {
                    debug_sys(LOG_ERR, "failed to call delele for %s, ret :%d\n", (char *)tp.key, ret);
                    continue;
                }
                break;
            default:
                break;
        }
    }

    return (0);
}

int add_txn_param(txn_param param, vector<txn_param> &params)
{
    params.push_back(param);
    return 0;
}

void free_txn_params(vector<txn_param> &params)
{
    for (vector<txn_param>::iterator it = params.begin();
         it != params.end(); it++)
    {
        if (!it->should_free)
        {
            continue;
        }

        my_free(it->key);
        my_free(it->value);
    }
}

int insert_key_value_basic(bdb_info *db, void *buf, size_t bufsize, void *value, size_t valuesize, DB_TXN *tid)
{
    int ret = 0;
    DBT key, data;
    DB     *dbp = db->dbp;              /* Database handle. */

    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));

    key.data = buf;
    key.size = bufsize;
    data.data = value;
    data.size = valuesize;
    data.flags = DB_DBT_USERMEM;

    pthread_rwlock_wrlock(&db->db_lock);
    ret = dbp->put(dbp, tid, &key, &data, 0);
    if (ret != 0)
    {
        dbp->err(dbp, ret, "DB->put");
        pthread_rwlock_unlock(&db->db_lock);
        debug_sys(LOG_ERR, "error to insert key :%s\n", (char *)buf);
        return ret;
    }

    pthread_rwlock_unlock(&db->db_lock);
    return 0;
}

int delete_key_basic(bdb_info *db, void *buf, size_t bufsize, DB_TXN *tid)
{
    int ret = -1;
    DBT key;
    DB     *dbp = db->dbp;              /* Database handle. */

    memset(&key, 0, sizeof(DBT));
    key.data = buf;
    key.size = bufsize;

    pthread_rwlock_wrlock(&db->db_lock);
    ret = dbp->del(dbp, tid, &key, 0);
    if (ret != 0)
    {
        dbp->err(dbp, ret, "DB->del");
        pthread_rwlock_unlock(&db->db_lock);
        return ret;
    }
    pthread_rwlock_unlock(&db->db_lock);

    return 0;
}

int get_key_value_basic(bdb_info *db, void *buf, size_t bufsize, void *value, size_t valuesize, DB_TXN *tid)
{
    int ret = 0;
    DBT key, data;
    DB     *dbp = db->dbp;              /* Database handle. */

    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));
    data.flags = DB_DBT_MALLOC;
    key.data = buf;
    key.size = bufsize;

    pthread_rwlock_rdlock(&db->db_lock);
    ret = dbp->get(dbp, tid, &key, &data, 0);
    pthread_rwlock_unlock(&db->db_lock);

    if (ret == 0)
    {
        /* Success. */
        memcpy(value, data.data, valuesize);
        my_free(data.data);
        return FOUND;
    }
    else if (ret == DB_NOTFOUND)    /* Not found. */
    {
        return NFOUND;
    }

    return ret;
}

int get_all_keys(bdb_info *db, vector<string> &vKeys, int max)
{
    int ret = 0;
    DBC *dbcp = NULL;
    DBT key, data;
    DB  *dbp = db->dbp;
    int i = 0;

    pthread_rwlock_rdlock(&db->db_lock);
    /* Acquire a cursor for the database. */
    if ((ret = dbp->cursor(dbp, NULL, &dbcp, 0)) != 0)
    {
        dbp->err(dbp, ret, "DB->cursor");
        goto err;
    }

    /* Initialize the key/data return pair. */
    memset(&key, 0, sizeof(key));
    memset(&data, 0, sizeof(data));

    /* Walk through the database and print out the key/data pairs. */
    while ((ret = dbcp->c_get(dbcp, &key, &data, DB_NEXT)) == 0)
    {
        //the max path length is less than 1024
        char keystr[1024] = {0};
        memcpy(keystr, key.data, key.size);
        vKeys.push_back(string(keystr, strlen(keystr)));
        i++;
        if (max != -1 && i >= max)
        {
            break;
        }
    }

err:
    if ((ret = dbcp->c_close(dbcp)) != 0)
    {
        dbp->err(dbp, ret, "DBcursor->close");
    }
    pthread_rwlock_unlock(&db->db_lock);

    return 0;
}

//db_init -- Initialize the environment.
static DB_ENV *db_init(char *home)
{
    const char *progname = "dircounter";
    DB_ENV *dbenv = NULL;
    int ret = 0;
    int max_locks = 1024000;

    if ((ret = db_env_create(&dbenv, 0)) != 0)
    {
        debug_sys(LOG_ERR, "%s: db_env_create: %s\n", progname, db_strerror(ret));
        return NULL;
    }

    dbenv->set_errfile(dbenv, stderr);
    dbenv->set_errpfx(dbenv, progname);
    dbenv->set_cachesize(dbenv, 0, 10 * 1024 * 1024, 0);
    dbenv->set_lg_max(dbenv, 20000);
    dbenv->mutex_set_max(dbenv, max_locks);

    u_int32_t maxp;
    dbenv->mutex_get_max(dbenv, &maxp);
    debug_sys(LOG_NOTICE, "max mutex for db %u\n", maxp);
    debug_sys(LOG_NOTICE, "begin to open db %s\n", home);

    ret = dbenv->open(dbenv, home,
                      DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN |
                      DB_INIT_MPOOL | DB_THREAD | DB_RECOVER, 0);

    debug_sys(LOG_NOTICE, "open db %s, ret : %d\n", home, ret);

#if 0
    ret = dbenv->open(dbenv, home,
                      DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
                      DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD, 0);

    ret = dbenv->open(dbenv, home,
                      DB_CREATE | DB_INIT_LOCK | DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD, 0);
#endif

    if (ret != 0)
    {
        dbenv->err(dbenv, ret, NULL);
        dbenv->close(dbenv, 0);
        debug_sys(LOG_ERR, "call dbenv open failed\n");
        return NULL;
    }
    return (dbenv);
}

int swap_insert(void *arg1, void *arg2)
{
    bdb_info *db = (bdb_info *)arg1;
    mem_obj *obj = (mem_obj *) arg2;

    debug_sys(LOG_NOTICE, "Swap for key %s\n", obj->path);
    insert_key_value(db, obj->path, obj->fi);
    return 0;
}

static void *swap_kv_process(void *arg)
{
    pthread_detach(pthread_self());
    while (1)
    {
        if (g_build_index_ok != 1)
        {
            my_sleep(1);
            continue;
        }

        my_sleep(120);

        debug_sys(LOG_DEBUG, "Check kv swap operations\n");
        if (get_mem() > g_config.max_memory * 8 / 10)
        {
            debug_sys(LOG_ERR, "use so many memory, process kv swap operations\n");
            swap_mem_2_db(swap_insert, arg);
        }
    }
    return NULL;
}

static void *sync_kv_process(void *arg)
{
    bdb_info *db = (bdb_info *)arg;
    DB     *dbp = db->dbp;              /* Database handle. */

    pthread_detach(pthread_self());
    while (1)
    {
        debug_sys(LOG_DEBUG, "process kv sync operations\n");
        pthread_rwlock_wrlock(&db->db_lock);
        dbp->sync(dbp, 0);
        pthread_rwlock_unlock(&db->db_lock);
        my_sleep(1000);
    }
    return NULL;
}

/*
 * deadlock -- Thread start function for lock_detect().
 */
static void *deadlock(void *arg)
{
    bdb_info *db = (bdb_info *)arg;
    DB_ENV *dbenv = db->dbenv;

    pthread_detach(pthread_self());

    pthread_t tid;
    tid = pthread_self();
    debug_sys(LOG_DEBUG, "deadlock thread starting: tid: %lu\n", (u_long)tid);

    for (;;)
    {
        debug_sys(LOG_DEBUG, "process kv deadlock detection operations\n");
        pthread_rwlock_wrlock(&db->db_lock);
        (void)dbenv->lock_detect(dbenv, 0, DB_LOCK_YOUNGEST, NULL);
        pthread_rwlock_unlock(&db->db_lock);
        my_sleep(1000);
    }

    /* NOTREACHED */
    return (NULL);
}

void *logfile_thread(void *arg)
{
    pthread_detach(pthread_self());

    int ret;
    char **begin, **list;

    bdb_info *db = (bdb_info *)arg;
    DB_ENV *dbenv = db->dbenv;

    dbenv->errx(dbenv, "Log file removal thread: %lu", (u_long)pthread_self());

    /* Check once every 5 minutes. */
    for (;;)
    {
        if (g_build_index_ok != 1)
        {
            my_sleep(300);
            continue;
        }

        debug_sys(LOG_DEBUG, "process kv logfile cleanup operations\n");
        my_sleep(300);

        /* Get the list of log files. */
        pthread_rwlock_wrlock(&db->db_lock);
        if ((ret = dbenv->log_archive(dbenv, &list, DB_ARCH_ABS | DB_ARCH_LOG)) != 0)
        {
            dbenv->err(dbenv, ret, "DB_ENV->log_archive");
            pthread_rwlock_unlock(&db->db_lock);
            continue;
        }
        pthread_rwlock_unlock(&db->db_lock);

        if (list == NULL)
        {
            debug_sys(LOG_ERR, "list is emtpy\n");
        }

        /* Remove the log files. */
        if (list != NULL)
        {
            char **next = NULL;
            for (begin = list; *list != NULL; ++list)
            {
                debug_sys(LOG_DEBUG, "get logfile :%s\n", *list);
                next = list + 1;
                if (*next == NULL)
                {
                    break;
                }

                debug_sys(LOG_DEBUG, "remove logfile :%s\n", *list);
                if ((ret = remove(*list)) != 0)
                {
                    dbenv->err(dbenv,
                               ret, "remove %s", *list);
                    my_free(begin);
                    break;
                }
            }
            my_free(begin);
        }
    }

    return NULL;
}

