#ifndef _KEYVALUE_H
#define _KEYVALUE_H

#include "headercxx.h"
#include <db.h>

/*
    KV_DIRECT means insert the records into db directly.
    KV_INDIRECT means insert into memory.
*/
#define KV_DB     1
#define KV_MEM    2

enum DB_TYPE
{
    INSERT = 0,
    DELETE,
    GET,
};

enum BDB_TYPE
{
    HASH = 0,
    BTREE,
};

typedef struct txn_param
{
    //if the key and value should be freed.
    bool    should_free;
    //type
    enum DB_TYPE type;
    //key
    void    *key;
    size_t   keysize;
    //value
    void    *value;
    size_t   valuesize;
} txn_param;

typedef struct bdb_info
{
    char *progname;
    char dbdir[MAX_PATH];
    char dbname[MAX_PATH];
    enum BDB_TYPE type;         /* HASH or Btree*/
    DB   *dbp;                  /* Database handle. */
    DB_ENV *dbenv;              /* Database environment. */
    pthread_rwlock_t db_lock;
} bdb_info;

/*
    return 0 -- succ
    return <0 --failed
*/
bdb_info *init_kv_storage(char *dir, char *name, enum BDB_TYPE type, int rm, int swap);
int deinit_kv_storage(bdb_info *db);

/*
    return 0 -- succ
    return <0 --failed
*/
int insert_key_value(bdb_info *db, char *key, fileinfo value);

/*
    return 1 -- success
    return 0 -- the key does not exist
    return <0 -- failed
*/
int get_key_value(bdb_info *db, char *key, fileinfo *value);

/*
    return 0 -- succ
    return <0 --failed
*/
int delete_key(bdb_info *db, char *key);

/*
    return 0 -- succ
    return <0 --failed
*/
int insert_key_value_cache(bdb_info *db, char *key, fileinfo value);

/*
    return 1 -- success
    return 0 -- the key does not exist
    return <0 -- failed
*/
int get_key_value_cache(bdb_info *db, char *key, fileinfo *value);

/*
    return 0 -- succ
    return <0 --failed
*/
int delete_key_cache(bdb_info *db, char *key);

int insert_key_value_basic(bdb_info *db, void *buf, size_t bufsize, void *value, size_t valuesize, DB_TXN *tid);

int delete_key_basic(bdb_info *db, void *buf, size_t bufsize, DB_TXN *tid);

int get_key_value_basic(bdb_info *db, void *buf, size_t bufsize, void *value, size_t valuesize, DB_TXN *tid);

int get_all_keys(bdb_info *db, vector<string> &vKeys, int max);

#if 0
/*
    process multi operations in on transaction. check the returned value
    return 0 -- succ
    return <0 --failed
*/
int process_db_txn(bdb_info *db, vector<txn_param> params, int flag);
#endif

int process_db_batch(bdb_info *db, vector<txn_param> params);
int add_txn_param(txn_param param, vector<txn_param> &params);
void free_txn_params(vector<txn_param> &params);

#endif
