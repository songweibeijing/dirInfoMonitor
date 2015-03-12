#include <algorithm>
#include "header.h"
#include "headercxx.h"
#include "inotifytools.h"
#include "inotify.h"
#include "inotify-nosys.h"
#include "inotifytools_p.h"
#include "inotify_process.h"
#include "monitor_dir.h"
#include "kv.h"
#include "util.h"
#include "log.h"
#include "bio.h"
#include "atomic.h"
#include "config.h"
#include "cJSON.h"

#include <set>
#include <string>
#include <vector>
#include <unordered_map>

using namespace std;

#define ADD 1
#define DEL 0
#define UPD 2

#define COUNTER_ONLY 0
#define COUNTER_SIZE 1

#define MAX_BUILD_THREADS 128
#define MAX_THREADS_FOR_BUILD 128

extern bdb_info *g_hash_db;
extern bdb_info *g_db;
extern pthread_rwlock_t g_action_lock;
extern config g_config;

//process handle definition.
typedef int (*inotify_process)(char *file, int event, int type, void *argv);
typedef unordered_map <int, inotify_process> eventFuncMap;
eventFuncMap g_funcs(100);

strCharhashMap g_sym_dirs(1024);             //record the system links in memory.
pthread_mutex_t g_sym_dir_lock = PTHREAD_MUTEX_INITIALIZER;
static string g_move_dir;

typedef struct inotify_item
{
    char *path;
    int  eventmask;
    int  type;      //0: only counter, 1: need file size
} inotify_item;

static int __build_directory_index(void *arg);
static int build_directorys_index(monitor_dirs *md, vector<string> &vdirs, unsigned int max_threads, atomic_t counter);

int add_notify_dir(const char *dir, int events, int level, char **exclude_list)
{
    debug_sys(LOG_NOTICE, "dir :%s, level: %d\n", dir, level);
    int ret = inotifytools_watch_recursively_level(dir, events, level, exclude_list);

    if (!ret)
    {
        if (inotifytools_error() == ENOSPC)
        {
            debug_sys(LOG_ERR, "Failed to watch %s; upper limit on inotify "
                      "watches reached!\n", dir);
            debug_sys(LOG_ERR, "Please increase the amount of inotify watches "
                      "allowed per user via `/proc/sys/fs/inotify/"
                      "max_user_watches'.\n");
            return -1;
        }
        else
        {
            debug_sys(LOG_ERR, "Couldn't watch %s: %s\n", dir,
                      strerror(inotifytools_error()));
            return -2;
        }
    }

    return 0;
}

static int update_all_parents_monitor_info(char *path, int action, void *delta)
{
    int ret = ERROR;
    vector<string> vDirs;
    vDirs.clear();
    fileinfo *fi = (fileinfo *)delta;

    debug_sys(LOG_DEBUG, "begin to modify file :%d, :%s, size:%lld, filenm:%lld\n",
              action, path, fi->filesz, fi->filenm);

    //update all parent statistic info
    get_all_parent_dir(path, vDirs);
    for (vector<string>::iterator it = vDirs.begin();
         it != vDirs.end(); it++)
    {
        char *dir = (char *)(*it).c_str();
        if (find_update_monitor_dir(g_md, dir, fi, action) == FOUND)
        {
            ret = SUCC;
        }
    }

    return ret;
}

static int update_file_num(char *path, int action, fileinfo &newinfo)
{
    fileinfo delta = {0, 0}, old = {0, 0};

    newinfo.filesz = 0;
    newinfo.filenm = 0;

    //try to find the old values
    int ret = get_key_value_cache(g_hash_db, path, &old);
    if (action == ADD)
    {
        if (ret == 1) //old value exists, update it
        {
            delta.filenm = 1 - old.filenm;
        }
        else //otherwise, add it
        {
            delta.filenm = 1;
        }
        newinfo.filenm = 1;
    }
    else if (action == DEL)
    {
        if (ret == 1) //old value exists, delete it.
        {
            delta.filenm = 1;
        }
        else //otherwise, no change it.
        {
            delta.filenm = 0;
        }
        newinfo.filenm = 0;
    }

    //no change
    if (delta.filenm == 0 && delta.filesz == 0)
    {
        return 0;
    }

    if (update_all_parents_monitor_info(path, action, &delta) != 0)
    {
        debug_sys(LOG_ERR, "insert file count for path:%s failed\n", path);
        return -1;
    }

    if (action == ADD)
    {
        insert_key_value_cache(g_hash_db, path, newinfo);
    }
    else if (action == DEL)
    {
        delete_key_cache(g_hash_db, path);
    }

    return 0;
}

static int update_file_num_and_size(char *path, int action, fileinfo &newinfo)
{
    int ret = 0;
    int64_t fz = 0;
    fileinfo delta = {0, 0}, old = {0, 0};

    //try to find the old values
    ret = get_key_value_cache(g_hash_db, path, &old);
    if (action == ADD)
    {
        fz = file_size(path);
        if (fz < 0)
        {
            //debug_sys(LOG_ERR, "get file size for path :%s failed\n", path);
            return -1;
        }
        if (ret == 1) //old value exists, update it
        {
            delta.filenm = 1 - old.filenm;
            delta.filesz = fz - old.filesz;
        }
        else //otherwise, add it
        {
            delta.filenm = 1;
            delta.filesz = fz;
        }
        newinfo.filenm = 1;
        newinfo.filesz = fz;
    }
    else if (action == DEL)
    {
        if (ret == 1) //old value exists, delete it.
        {
            delta.filenm = 1;
            delta.filesz = old.filesz;
        }
        else //otherwise, no change it.
        {
            delta.filenm = 0;
            delta.filesz = 0;
        }
        newinfo.filenm = 0;
        newinfo.filesz = 0;
    }

    //no change
    if (delta.filenm == 0 && delta.filesz == 0)
    {
        return 0;
    }

    if (update_all_parents_monitor_info(path, action, &delta) != 0)
    {
        debug_sys(LOG_ERR, "insert file count for path:%s failed\n", path);
        //return -1;
    }

    if (action == ADD)
    {
        insert_key_value_cache(g_hash_db, path, newinfo);
    }
    else if (action == DEL)
    {
        delete_key_cache(g_hash_db, path);
    }
    return 0;
}

static int insert_file(char *path, int type)
{
    fileinfo newinfo = {0, 0};
    if (type == COUNTER_SIZE)
    {
        update_file_num_and_size(path, ADD, newinfo);
    }
    else if (type == COUNTER_ONLY)
    {
        update_file_num(path, ADD, newinfo);
    }
    return newinfo.filesz;
}

static int delete_file(char *path, int type)
{
    fileinfo newinfo;
    if (type == COUNTER_SIZE)
    {
        update_file_num_and_size(path, DEL, newinfo);
    }
    else if (type == COUNTER_ONLY)
    {
        update_file_num(path, DEL, newinfo);
    }
    return 0;
}

static int delete_dir(char *dir, int type)
{
    string strdir = string(dir, strlen(dir));
    debug_sys(LOG_NOTICE, "begin to delete directory: %s\n", dir);

    del_monitor_dir(g_md, dir);

    //record the deleted dir.
    pthread_mutex_lock(&g_delete_dir_lock);
    add_key_set(g_delete_dir, strdir);
    pthread_mutex_unlock(&g_delete_dir_lock);

    return 0;
}

static int is_sym_dir(char *buf)
{
    int sym_dir = 0;
    struct stat64 my_stat;
    if ((lstat64(buf, &my_stat) == 0) && (S_ISLNK(my_stat.st_mode)))
    {
        sym_dir = 1;
    }
    return sym_dir;
}

int process_sym_link(char *file, int &eventmask)
{
    int special = 0, oper = NOOP;
    string filestr = string(file, strlen(file));
    if (eventmask == IN_CREATE)
    {
        if (is_sym_dir(file))
        {
            eventmask = IN_CREATE | IN_ISDIR;
            oper = ADD;
        }
    }
    else if ((eventmask == IN_DELETE)
             || (eventmask == (IN_DELETE | IN_ISDIR))
             || (eventmask == IN_DELETE_SELF))
    {
        oper = DELE;
    }

    pthread_mutex_lock(&g_sym_dir_lock);
    if (oper == ADD)
    {
        add_key_set(g_sym_dirs, filestr);
        special = 1;
    }
    else if (oper == DELE)
    {
        special = del_key_set(g_sym_dirs, filestr);
    }
    pthread_mutex_unlock(&g_sym_dir_lock);

    if (special && (eventmask == IN_DELETE))
    {
        eventmask = IN_DELETE | IN_ISDIR;
    }

    return special;
}

int do_create_file(char *file, int eventmask, int type, void *argv)
{
    int ret = 0;

    debug_sys(LOG_DEBUG, "IN_CREATE for file %s\n", file);
    ret = insert_file(file, type);

    return ret;
}

static bool cmp_string_length(const string &v1, const string &v2)
{
    return v1.length() > v2.length();//longest path first.
}

static int do_posted_create_dir(void *arg)
{
    int level = 0;
    char path[256] = {0};
    char *file = NULL;
    string debug_string = "";
    char debug_str[1024] = {0};
    atomic_t index_threads_num;
    atomic_set(&index_threads_num, 0);

    vector<path_level> vSubdir;
    vector<string> vResdir;
    vSubdir.clear();
    vResdir.clear();

    debug_sys(LOG_DEBUG, "begin to do posted created dir\n");

    inotify_item *item = (inotify_item *)arg;
    if (arg == NULL)
    {
        debug_sys(LOG_ERR, "empty inotify item\n");
        return -1;
    }

    file = item->path;
    if (!file || !file_exist(file))
    {
        debug_sys(LOG_ERR, "file is NULL or file does not exist\n");
        return 0;
    }

    level = find_monitor_file_level(g_md, file, -1);
    get_all_subdir(file, level, vSubdir);
    for (vector<path_level>::iterator it = vSubdir.begin();
         it != vSubdir.end(); it++)
    {
        memset(path, 0, sizeof(path));
        path_level pl = *it;
        if (is_exclude_dir((char *)pl.path.c_str(), &g_md->ex_dirs) == FOUND)
        {
            snprintf(debug_str, 1024, "\tpath %s is in excluded list\n", pl.path.c_str());
            debug_string += string(debug_str);
            add_sub_exclude_dir(file, (char *)pl.path.c_str(), &g_md->ex_dirs);
        }
        else
        {
            snprintf(debug_str, 1024, "\tsubdir : %s, level : %d\n", it->path.c_str(), it->level);
            debug_string += string(debug_str);
            memcpy(path, it->path.c_str(), it->path.length());
            add_monitor_dir_inotify(g_md, path, it->level, item->type, 0);
            vResdir.push_back(pl.path);
        }
    }
    debug_sys(LOG_DEBUG, "process dir %s\n%s", file, debug_string.c_str());

    if (vResdir.size() > 0)
    {
        sort(vResdir.begin(), vResdir.end(), cmp_string_length);
    }

    build_directorys_index(NULL, vResdir, MAX_THREADS_FOR_BUILD, index_threads_num);

    my_free(item->path);
    my_free(item);

    return 0;
}

int do_create_dir(char *file, int eventmask, int type, void *argv)
{
    int ret = 0, special = (int)argv;
    inotify_item *item = NULL;

    debug_sys(LOG_DEBUG, "IN_CREATE for dir %s\n", file);
    if (special == 1)
    {
        debug_sys(LOG_DEBUG, "it is symlink, IN_CREATE|IN_ISDIR for file %s\n", file);
    }
    if (is_exclude_dir(file, &g_md->ex_dirs) == FOUND)
    {
        debug_sys(LOG_DEBUG, "dir %s is in excluded list\n", file);
        return ret;
    }

    ret = add_monitor_dir_inotify(g_md, file, -1, type, special);
    if (ret != 0)
    {
        debug_sys(LOG_ERR, "add notify dir for file %s failed\n", file);
        return ret;
    }
    item = (inotify_item *)calloc(1, sizeof(inotify_item));
    if (item == NULL)
    {
        debug_sys(LOG_ERR, "malloc error for %s\n", file);
        return -1;
    }
    item->path = strdup(file);
    if (item->path == NULL)
    {
        my_free(item);
        debug_sys(LOG_ERR, "malloc error for %s\n", file);
        return -1;
    }
    item->type = type;
    debug_sys(LOG_DEBUG, "request for posted handle for file %s\n", file);
    bio_create_job(POSTED_HANDLE, do_posted_create_dir, (void *)item);

    return 0;
}

int do_close_write(char *file, int eventmask, int type, void *argv)
{
    int ret = 0;

    debug_sys(LOG_DEBUG, "IN_CLOSE_WRITE for file %s\n", file);
    if (type == COUNTER_SIZE)
    {
        ret = insert_file(file, type);
    }

    return ret;
}

int do_delete_file(char *file, int eventmask, int type, void *argv)
{
    int ret = 0;

    debug_sys(LOG_DEBUG, "IN_DELETE for file %s\n", file);
    ret = delete_file(file, type);

    return ret;
}

int do_delete_dir_notify(char *file, int eventmask, int type, void *argv)
{
    int ret = 0;

    debug_sys(LOG_DEBUG, "IN_DELETE_SELF for dir %s\n", file);
    if (del_monitor_dir_inotify(g_md, file) != SUCC)
    {
        return -1;
    }

    return ret;
}

int do_delete_dir(char *file, int eventmask, int type, void *argv)
{
    int ret = 0, special = (int)argv;

    debug_sys(LOG_DEBUG, "IN_DELETE for dir %s\n", file);
    if (special == 1)
    {
        debug_sys(LOG_DEBUG, "%s is symlink, remove watch it, but keep its object\n", file);
        del_dir_inotify(file);
        return 0;
    }
    ret = delete_dir(file, type);

    return ret;
}

int do_move_file_from(char *file, int eventmask, int type, void *argv)
{
    int ret = 0;

    debug_sys(LOG_DEBUG, "IN_MOVED_FROM for file %s\n", file);
    ret = delete_file(file, type);

    return ret;
}

int do_move_file_to(char *file, int eventmask, int type, void *argv)
{
    int ret = 0;

    debug_sys(LOG_DEBUG, "IN_MOVED_TO for file %s\n", file);
    ret = insert_file(file, type);

    return ret;
}


/*
    move dir from and move dir to, it is temp situation.
    we just modify the watch mechanism, and do not
    touch the g_md in memory.
*/
int do_move_dir_from(char *file, int eventmask, int type, void *argv)
{
    int ret = 0;

    debug_sys(LOG_DEBUG, "IN_MOVED_FROM for dir %s\n", file);
    g_move_dir = string(file, strlen(file));

    return ret;
}

int do_move_dir_to(char *file, int eventmask, int type, void *argv)
{
    int from_is_excl, to_is_excl, ret = 0;
    monitor_dir mditem;
    inotify_item *item = NULL;

    from_is_excl = to_is_excl = 0;
    if (g_move_dir.length() == 0)
    {
        debug_sys(LOG_ERR, "Must get move dir from event first for file %s\n", file);
        return -1;
    }

    from_is_excl = is_exclude_dir((char *)g_move_dir.c_str(), &g_md->ex_dirs);
    to_is_excl = is_exclude_dir(file, &g_md->ex_dirs);

    debug_sys(LOG_DEBUG, "IN_MOVED_TO for dir %s, from %s, to %s\n"
              , file, from_is_excl == NFOUND ? "normal" : "exclude", to_is_excl == NFOUND ? "normal" : "exclude");

    if (from_is_excl == NFOUND && to_is_excl == NFOUND)
    {
        inotifytools_replace_filename((char *)g_move_dir.c_str(), file);
    }
    else if (from_is_excl == FOUND && to_is_excl == NFOUND)
    {
        g_move_dir = "";
        memset(&mditem, 0, sizeof(mditem));
        if (find_monitor_dir(g_md, file, &mditem) == FOUND)
        {
            add_dir_inotify(g_md, file, mditem.directory_level);
        }
        else
        {
            ret = add_monitor_dir_inotify(g_md, file, -1, type, 1);
            if (ret != SUCC)
            {
                debug_sys(LOG_ERR, "Failed to add_monitor_dir_inotify for file %s\n", file);
            }
        }

        item = (inotify_item *)calloc(1, sizeof(inotify_item));
        if (item == NULL)
        {
            debug_sys(LOG_ERR, "malloc error for %s\n", file);
            return -1;
        }
        item->path = strdup(file);
        if (item->path == NULL)
        {
            my_free(item);
            debug_sys(LOG_ERR, "malloc error for %s\n", file);
            return -1;
        }
        item->type = type;
        debug_sys(LOG_DEBUG, "request for posted handle for file %s\n", file);
        bio_create_job(POSTED_HANDLE, do_posted_create_dir, (void *)item);
    }
    else if (from_is_excl == NFOUND && to_is_excl == FOUND)
    {
        del_dir_inotify((char *)g_move_dir.c_str());
    }

    g_move_dir = "";
    return ret;
}

void *dir_change_notify_process(void *arg)
{
    int error_times = 4;
    vector<monitor_dir> vOlddirs;
    vector<monitor_dir> vNewdirs;
    map<string, int> m_missdir;
    m_missdir.clear();

    pthread_detach(pthread_self());
    while (1)
    {
        my_sleep(g_config.check_interval);

        //it only works when builing index is ok.
        if (g_build_index_ok == 0)
        {
            continue;
        }

        debug_sys(LOG_DEBUG, "Begin to loop all dirs and change the missing one\n");

        vOlddirs.clear();
        vNewdirs.clear();

        sort_monitor_dirs(g_md, vOlddirs);
        get_monitor_dir_from_config(g_config.default_monitor_file, g_md);
        sort_monitor_dirs(g_md, vNewdirs);

        if (vOlddirs.size() == vNewdirs.size())
        {
            debug_sys(LOG_DEBUG, "No changes of directories, try to do it again\n");
            m_missdir.clear();
            continue;
        }

        for (vector<monitor_dir>::iterator it = vNewdirs.begin(); it != vNewdirs.end();
             it++)
        {
            int found = 0;
            string newpath = string(it->dir_name, strlen(it->dir_name));
            for (vector<monitor_dir>::iterator it_old = vOlddirs.begin();
                 it_old != vOlddirs.end(); it_old++)
            {
                if (newpath == string(it_old->dir_name, strlen(it_old->dir_name)))
                {
                    found = 1;
                    break;
                }
            }

            if (found == 0) //can not find it
            {
                map<string, int>::iterator itmap = m_missdir.find(newpath);
                if (itmap == m_missdir.end())
                {
                    m_missdir.insert(make_pair(newpath, 1));
                }
                else
                {
                    itmap->second++;
                }
            }
            else
            {
                m_missdir.erase(newpath);
            }
        }

        for (map<string, int>::iterator it = m_missdir.begin();
             it != m_missdir.end(); it++)
        {
            char *path = (char *)it->first.c_str();
            if (it->second >= error_times)
            {
                debug_sys(LOG_DEBUG, "Dir :%s exists, but not in inotify system, so add it into the notify system, add it\n", path);
                __build_directory_index((void *)path);
            }
        }
    }
    return NULL;
}

fileinfo count_dir_fileinfo(char *dir, monitor_dirs *md)
{
    fileinfo fi = {0, 0};
    char buf[4096] = {0};
    DIR *top_defer_dir = NULL;
    top_defer_dir = opendir(dir);
    struct dirent *dp = NULL;

    while (top_defer_dir)
    {
        dp = (struct dirent *)readdir64(top_defer_dir);
        if (dp == NULL || strlen(dp->d_name) == 0)
        {
            closedir(top_defer_dir);
            top_defer_dir = NULL;
            break;
        }

        if (dp->d_name[0] == '.')
        {
            continue;
        }

        if (dir[strlen(dir) - 1] == '/')
        {
            snprintf(buf, sizeof(buf), "%s%s", dir, dp->d_name);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s/%s", dir, dp->d_name);
        }

        if (dp->d_type == DT_DIR)
        {
            fileinfo fidir = {0, 0};
            if (!is_exclude_dir(buf, &md->ex_dirs))
            {
                monitor_dir mditem;
                if (find_monitor_dir(md, buf, &mditem) == FOUND)
                {
                    memcpy(&fidir, &mditem.fi, sizeof(fileinfo));
                }
            }
            fi.filenm += fidir.filenm;
            fi.filesz += fidir.filesz;
        }
        else if (dp->d_type != DT_DIR)
        {
            struct stat64 statbuf;
            int ret = lstat64(buf, &statbuf);
            if (ret != 0)
            {
                continue;
            }

            if (S_ISLNK(statbuf.st_mode))
            {
                struct stat64 my_stat2;
                if (stat64(buf, &my_stat2) == 0 && S_ISDIR(my_stat2.st_mode))
                {
                    fileinfo fidir = {0, 0};
                    if (!is_exclude_dir(buf, &md->ex_dirs))
                    {
                        monitor_dir mditem;
                        if (find_monitor_dir(md, buf, &mditem) == FOUND)
                        {
                            memcpy(&fidir, &mditem.fi, sizeof(fileinfo));
                        }
                    }
                    fi.filenm += fidir.filenm;
                    fi.filesz += fidir.filesz;
                }
                else
                {
                    fi.filenm++;
                    fi.filesz += statbuf.st_size;
                }
            }
            else
            {
                fi.filenm++;
                fi.filesz += statbuf.st_size;
            }
        }
    }

    return fi;
}

int check_one_dir(char *path, map<string, int> &errordir)
{
    int type = 0, error_time = 0;
    monitor_dir md;
    fileinfo dirmem, dirmem_2;
    string dir = string(path, strlen(path));
    memset(&md, 0, sizeof(monitor_dir));
    memset(&dirmem, 0, sizeof(fileinfo));
    memset(&dirmem_2, 0, sizeof(fileinfo));

    find_monitor_dir(g_md, path, &md);
    type = md.is_counter_size;
    dirmem = md.fi;
    dirmem_2 = count_dir_fileinfo(path, g_md);

    if (dirmem.filenm != dirmem_2.filenm
        || dirmem.filesz != dirmem_2.filesz)
    {
        map<string, int>::iterator itmap = errordir.find(dir);
        if (itmap == errordir.end())
        {
            errordir.insert(make_pair(dir, 1));
        }
        else
        {
            itmap->second++;
            error_time = itmap->second;
        }
    }
    else
    {
        errordir.erase(dir);
    }

    if (error_time > 3)
    {
        debug_sys(LOG_DEBUG, "Dir :%s exists, but not in inotify system, so add it into the notify system, add it\n", path);
        __build_directory_index((void *)path);
        errordir.erase(dir);
    }

    return 0;
}

void *dir_check_process(void *arg)
{
    vector<monitor_dir> vNewdirs;
    map<string, int> m_errordir;
    m_errordir.clear();

    pthread_detach(pthread_self());
    while (1)
    {
        my_sleep(g_config.check_interval);
        if (g_build_index_ok == 0)
        {
            continue;
        }

        debug_sys(LOG_DEBUG, "Begin to Check one dir\n");
        vNewdirs.clear();
        sort_monitor_dirs(g_md, vNewdirs);

        if (vNewdirs.size() == 0)
        {
            debug_sys(LOG_DEBUG, "No changes of directories, try to do it again\n");
            m_errordir.clear();
            continue;
        }

        for (vector<monitor_dir>::iterator it = vNewdirs.begin(); it != vNewdirs.end();
             it++)
        {
            check_one_dir(it->dir_name, m_errordir);
            my_sleep(g_config.check_one_folder_interval);
        }
    }
    return NULL;
}

static void create_worker(void * (*func)(void *), void *thread)
{
    pthread_t       tid;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&tid, &attr, func, thread)) != 0)
    {
        debug_sys(LOG_ERR, "call pthread_create error:%d\n", errno);
    }
}

static void traverse_dir(char *dir, int type)
{
    char buf[4096] = {0};
    DIR *top_defer_dir = NULL;
    top_defer_dir = opendir(dir);
    struct dirent *dp = NULL;

    debug_sys(LOG_DEBUG, "Begin to process dir %s\n", dir);

    while (top_defer_dir)
    {
        dp = (struct dirent *)readdir64(top_defer_dir);

        if (dp == NULL || strlen(dp->d_name) == 0)
        {
            closedir(top_defer_dir);
            top_defer_dir = NULL;
            break;
        }

        if (dp->d_name[0] == '.')
        {
            continue;
        }

        if (dir[strlen(dir) - 1] == '/')
        {
            snprintf(buf, sizeof(buf), "%s%s", dir, dp->d_name);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s/%s", dir, dp->d_name);
        }

        debug_sys(LOG_DEBUG, "Begin to process file %s\n", buf);
        if (dp->d_type == DT_DIR)
        {
            continue;
        }
        else if (dp->d_type != DT_DIR)
        {
            debug_sys(LOG_DEBUG, "Begin to insert file %s\n", buf);
            insert_file(buf, type);
        }
    }
}

static int __build_directory_index(void *arg)
{
    string key = "";
    char *dir = (char *)arg;
    monitor_dir dirinfotmp;
    monitor_dir *dirinfo = &dirinfotmp;

    if (find_monitor_dir(g_md, dir, dirinfo) == FOUND)
    {
        key = string(dirinfo->dir_name, strlen(dirinfo->dir_name));
        pthread_mutex_lock(&g_delete_dir_lock);
        del_key_set(g_delete_dir, key);
        pthread_mutex_unlock(&g_delete_dir_lock);

        debug_sys(LOG_DEBUG, "begin to update dir info for:%s\n", dirinfo->dir_name);
        traverse_dir(dirinfo->dir_name, dirinfo->is_counter_size);
    }
    return 0;
}

typedef struct build_thread_param
{
    void **args;
    int num;

    atomic_t *p_counter;
} build_thread_param;

//this fucntion will not free the memory related to arg
static void *build_directory_index(void *arg)
{
    pthread_detach(pthread_self());
    build_thread_param *param = (build_thread_param *)arg;

    if (param != NULL && param->args != NULL)
    {
        for (int i = 0; i < param->num; i++)
        {
            __build_directory_index(param->args[i]);
        }
    }
    atomic_sub(1, param->p_counter);
    return NULL;
}

static int build_directorys_index(monitor_dirs *md, vector<string> &vdirs, unsigned int max_threads, atomic_t counter)
{
    build_thread_param *param = NULL;
    int thread_num = 0, div = 0, left = 0, left_index = 0;

    //reset the num to 0.
    atomic_set(&counter, 0);

    if (vdirs.size() == 0)
    {
        return 0;
    }

    vector<string>::iterator it = vdirs.begin();
    thread_num = vdirs.size() > max_threads ? max_threads : vdirs.size();
    param = (build_thread_param *)calloc(thread_num, sizeof(build_thread_param));
    if (param == NULL)
    {
        printf("malloc failed\n");
        return -1;
    }

    div = vdirs.size() / thread_num;
    left = vdirs.size() - div * thread_num;

    debug_sys(LOG_DEBUG, "thread_num : %d, div : %d, left : %d\n", thread_num, div, left);
    for (int i = 0; i < thread_num; i++)
    {
        int objsize = div;
        if (left_index < left)
        {
            objsize++;
            left_index++;
        }
        build_thread_param *tmp = param + i;
        tmp->args = (void **) calloc(objsize, sizeof(void *));
        if (tmp->args == NULL)
        {
            debug_sys(LOG_ERR, "malloc failed\n");
            goto failed;
        }

        tmp->num = objsize;
        for (int z = 0; z < objsize; z++)
        {
            tmp->args[z] = (void *)strdup(it->c_str());
            if (tmp->args[z] == NULL)
            {
                debug_sys(LOG_ERR, "malloc failed\n");
                goto failed;
            }
            it++;
        }
        tmp->p_counter = &counter;

        atomic_add(1, &counter);
        create_worker(build_directory_index, (void *)tmp);
    }

    while (1)
    {
        int thread_num = atomic_read(&counter);
        debug_sys(LOG_DEBUG, "thread num %d\n", thread_num);
        if (thread_num == 0)
        {
            debug_sys(LOG_NOTICE, "build index ok, all threads return.\n");
            break;
        }
        else
        {
            my_usleep(10);
            continue;
        }
    }

failed:
    if (param)
    {
        for (int i = 0; i < thread_num; i++)
        {
            build_thread_param *tmp = param + i;
            if (tmp->args)
            {
                for (int j = 0; j < tmp->num; j++)
                {
                    my_free(tmp->args[j]);
                }
                my_free(tmp->args);
            }
        }

        my_free(param);
    }

    return 0;
}

static int process_monitor_dir(char *dir, int level, std::vector<std::string> &vstrExcludes, int is_counter_size, void *argv)
{
    char path[256] = {0};
    string wildchar = "(.*)";
    string exdirpattern = "";
    vector<path_level> vSubdir;
    vSubdir.clear();

    string debug_string = "";
    char debug_str[1024] = {0};

    monitor_dirs *md = (monitor_dirs *)argv;
    if (md == NULL)
    {
        return -1;
    }

    if (!file_exist(dir))
    {
        return 0;
    }

    //add exclude dir here
    for (vector<string>::iterator it = vstrExcludes.begin();
         it != vstrExcludes.end(); it++)
    {
        exdirpattern = "^" + string(dir) + "/" + wildchar + *it + wildchar + "$";
        add_exclude_pattern((char *)exdirpattern.c_str(), &md->ex_dirs);
    }

    get_all_subdir(dir, level, vSubdir);
    for (vector<path_level>::iterator it = vSubdir.begin();
         it != vSubdir.end(); it++)
    {
        memset(path, 0, sizeof(path));
        path_level pl = *it;
        if (is_exclude_dir((char *)pl.path.c_str(), &md->ex_dirs) == FOUND)
        {
            snprintf(debug_str, 1024, "\t%s\n", pl.path.c_str());
            debug_string += string(debug_str);
            add_sub_exclude_dir(dir, (char *)pl.path.c_str(), &md->ex_dirs);
        }
    }
    debug_sys(LOG_DEBUG, "dir %s has excluded list:\n%s", dir, debug_string.c_str());
    debug_string = "";

    for (vector<path_level>::iterator it = vSubdir.begin();
         it != vSubdir.end(); it++)
    {
        memset(path, 0, sizeof(path));
        path_level pl = *it;
        if (is_exclude_dir((char *)pl.path.c_str(), &md->ex_dirs) == NFOUND)
        {
            snprintf(debug_str, 1024, "\tsubdir:%s, level:%d\n", it->path.c_str(), it->level);
            debug_string += string(debug_str);
            memcpy(path, it->path.c_str(), it->path.length());
            add_monitor_dir_inotify(md, path, it->level, is_counter_size, 0);
        }
    }
    debug_sys(LOG_DEBUG, "dir %s has subdirs:\n%s", dir, debug_string.c_str());

    return 0;
}

typedef int (*cfg_handle_ex)(char *dir, int level, std::vector<std::string> &vstrExcludes, int is_counter_size, void *argv);
static int process_json_config_file(const char *file, cfg_handle_ex handle, void *cfg)
{
    int iret = 0;
    int ifilesize = 0;

    if ((ifilesize = file_size((char *)file)) < 0)
    {
        debug_sys(LOG_ERR, "error get file size!\n");
        return -1;
    }

    char *pbuf = (char *)malloc(ifilesize + 1);
    if (NULL == pbuf)
    {
        debug_sys(LOG_ERR, "error for malloc memory!\n");
        return -1;
    }

    FILE *fp = fopen(file, "rb");
    if (NULL == fp)
    {
        debug_sys(LOG_ERR, "open file: %s faild!\n", file);
        my_free(pbuf);
        return -1;
    }
    if ((int)fread(pbuf, 1, ifilesize, fp) != ifilesize)
    {
        debug_sys(LOG_ERR, "read file error : %d!\n", errno);
        fclose(fp);
        my_free(pbuf);
        return -1;
    }

    cJSON *pJSONroot = cJSON_Parse(pbuf);
    if (NULL == pJSONroot)
    {
        debug_sys(LOG_ERR, "illegal JSON file!\n");
        fclose(fp);
        my_free(pbuf);
        return -1;
    }

    cJSON *pTemp = NULL;
    cJSON *pJSONpath = NULL;
    cJSON *pJSONlevel = NULL;
    cJSON *pJSONExcludes = NULL;
    cJSON *pJSONiscountersize = NULL;

    int iArraySize = cJSON_GetArraySize(pJSONroot);
    int i, j = 0;
    for (i = 0; i < iArraySize; i++)
    {
        pTemp = cJSON_GetArrayItem(pJSONroot, i);
        if (NULL == pTemp)
        {
            debug_sys(LOG_ERR, "illegal JSON root!\n");
            continue;
        }

        pJSONpath = cJSON_GetArrayItem(pTemp, 0);
        pJSONlevel = cJSON_GetArrayItem(pTemp, 1);
        pJSONExcludes = cJSON_GetArrayItem(pTemp, 2);
        pJSONiscountersize = cJSON_GetArrayItem(pTemp, 3);
        if ((NULL == pJSONpath) ||
            (NULL == pJSONlevel) ||
            (NULL == pJSONiscountersize) ||
            (NULL == pJSONExcludes))
        {
            debug_sys(LOG_ERR, "illegal JSON object!\n");
            continue;
        }
        std::vector<std::string> vstrExludes;
        vstrExludes.clear();

        j = 0;

        for (j = 0; j < cJSON_GetArraySize(cJSON_GetObjectItem(pJSONExcludes, "excludes")); j++)
        {
            pTemp = cJSON_GetArrayItem(cJSON_GetObjectItem(pJSONExcludes, "excludes"), j);
            if ((NULL != pTemp) &&
                (0 != strlen(pTemp->valuestring)))
            {
                vstrExludes.push_back(pTemp->valuestring);
                printf("excluded file %s\n", pTemp->valuestring);
            }
        }

        if (handle(cJSON_GetObjectItem(pJSONpath, "path")->valuestring,
                   cJSON_GetObjectItem(pJSONlevel, "level")->valueint,
                   vstrExludes,
                   cJSON_GetObjectItem(pJSONiscountersize, "is_counter_size")->valueint,
                   cfg) != 0)
        {
            cerr << __FILE__ << " handle : " << pJSONpath->valuestring << " error" << endl;
            iret = -1;
            goto done;
        }

    }

done:
    if (NULL != pJSONroot)
    {
        cJSON_Delete(pJSONroot), pJSONroot = NULL;
    }
    if (NULL != fp)
    {
        fclose(fp), fp = NULL;
    }
    my_free(pbuf);
    return 0;
}

//the files or the directories wanted to be monitored contain coincidence.
int get_monitor_dir_from_config(const char *configfile, monitor_dirs *md)
{
    return process_json_config_file(configfile, process_monitor_dir, (void *)md);
}

static inotify_process find_ops(int eventmask)
{
    eventFuncMap::iterator it = g_funcs.find(eventmask);
    if (it != g_funcs.end())
    {
        return it->second;
    }
    else
    {
        return NULL;
    }
}

static int __process_fs_notify_item(char *file, int eventmask, int special)
{
    int type = 0;

    inotify_process func = find_ops(eventmask);
    if (func)
    {
        if (eventmask == IN_CREATE || eventmask == (IN_CREATE | IN_ISDIR)
            || eventmask == IN_CLOSE_WRITE || eventmask == IN_DELETE
            || eventmask == IN_MOVED_FROM || eventmask == IN_MOVED_TO)
        {
            type = find_monitor_file_type(g_md, file);
        }

        (*func)(file, eventmask, type, (void *)special);
    }
    else
    {
        debug_sys(LOG_ERR, "Unsupported event %s, %d\n",
                  inotifytools_event_to_str(eventmask), eventmask);
    }

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

    return hash;
}

static int process_fs_notify_item_threaded(void *arg)
{
    int ret = 0;
    inotify_item *item = (inotify_item *)arg;

    if (arg == NULL)
    {
        return -1;
    }

    debug_sys(LOG_DEBUG, "process file : %s, event :%d\n", item->path, item->eventmask);

    ret = __process_fs_notify_item(item->path, item->eventmask, 0);
    my_free(item->path);
    my_free(item);

    return ret;
}

static int process_fs_notify_item(void *arg)
{
    int ret = 0, thread_index = 0, special = 0;
    inotify_item *item = (inotify_item *)arg;

    if (arg == NULL)
    {
        return -1;
    }

    special = process_sym_link(item->path, item->eventmask);
    if ((item->eventmask & IN_ISDIR) == 0)
    {
        //no dir, use parellel.
        thread_index = RSHash(item->path, strlen(item->path)) % (BIO_NUM_OPS - HANDLE_INOTIFY_THREADED) + HANDLE_INOTIFY_THREADED;
        bio_create_job(thread_index, process_fs_notify_item_threaded, (void *)item);
        return 0;
    }

    wait_for_bio_threads();
    debug_sys(LOG_DEBUG, "process file : %s, event :%d\n", item->path, item->eventmask);
    ret = __process_fs_notify_item(item->path, item->eventmask, special);
    my_free(item->path);
    my_free(item);

    return ret;
}

static int inotify_event_convert(struct inotify_event *event, char *file, int *eventmask)
{
    char *dir = NULL, *eventstr = NULL;

    *eventmask = event->mask;
    dir = inotifytools_filename_from_wd(event->wd);
    eventstr = inotifytools_event_to_str(event->mask);
    if (dir == NULL || strlen(dir) == 0
        || eventstr == NULL || strlen(eventstr) == 0)
    {
        debug_sys(LOG_ERR, "get wrong event, drop it\n");
        return -1;
    }
    if (strlen(dir) > 1 && dir[strlen(dir) - 1] == '/')
    {
        dir[strlen(dir) - 1] = '\0';
    }
    if (event->mask != IN_DELETE_SELF)
    {
        snprintf(file, MAX_PATH, "%s/%s", dir, event->name);
    }
    else
    {
        snprintf(file, MAX_PATH, "%s", dir);
    }
    return 0;
}

static void *fs_notify_process(void *arg)
{
    inotify_item *item = NULL;
    char file[MAX_PATH];
    int eventmask;
    struct inotify_event *event = NULL;

    pthread_detach(pthread_self());

    while (1)
    {
        debug_sys(LOG_DEBUG, "Get one inotify info\n");

        memset(file, 0, sizeof(file));
        event = inotifytools_next_event(-1);
        if (!event)
        {
            debug_sys(LOG_ERR,  "%s\n", strerror(inotifytools_error()));
            continue;
        }

        if (inotify_event_convert(event, file, &eventmask) != 0)
        {
            debug_sys(LOG_ERR, "convert error for file %s event %d\n", file, eventmask);
            continue;
        }

        if (eventmask == IN_IGNORED)
        {
            continue;
        }

        item = (inotify_item *)calloc(1, sizeof(inotify_item));
        if (item == NULL)
        {
            debug_sys(LOG_ERR, "malloc error for %s\n", file);
            continue;
        }
        item->path = strdup(file);
        if (item->path == NULL)
        {
            my_free(item);
            debug_sys(LOG_ERR, "malloc error for %s\n", file);
            continue;
        }

        item->eventmask = eventmask;
        bio_create_job(HANDLE_INOTIFY, process_fs_notify_item, (void *)item);
    }

    return NULL;
}

static void register_op(int eventmask, inotify_process func)
{
    eventFuncMap::iterator it = g_funcs.find(eventmask);
    if (it == g_funcs.end())
    {
        g_funcs.insert(make_pair(eventmask, func));
    }
}

static void register_ops()
{
    register_op(IN_CREATE, do_create_file);
    register_op(IN_CREATE | IN_ISDIR, do_create_dir);
    register_op(IN_CLOSE_WRITE, do_close_write);
    register_op(IN_DELETE, do_delete_file);
    register_op(IN_DELETE_SELF, do_delete_dir_notify);
    register_op(IN_DELETE | IN_ISDIR, do_delete_dir);
    register_op(IN_MOVED_FROM, do_move_file_from);
    register_op(IN_MOVED_TO, do_move_file_to);
    register_op(IN_MOVED_FROM | IN_ISDIR, do_move_dir_from);
    register_op(IN_MOVED_TO | IN_ISDIR, do_move_dir_to);
}

static int increase_inotify_number(unsigned long long max_num, char *dirkey)
{
    char cmd[TMP_BUFSIZ] = {0};
    int ret = -1;

    while (1)
    {
        snprintf(cmd, TMP_BUFSIZ, "echo %llu > %s", max_num, dirkey);
        ret = system(cmd);
        if (ret != 0)
        {
            max_num = max_num / 2;
            if (max_num <= 1)
            {
                return -1;
            }
        }
        else
        {
            break;
        }
    }
    debug_sys(LOG_NOTICE, "max inotify num for %s to :%llu\n", dirkey, max_num);
    return 0;
}

static int increase_inotify_watches()
{
    increase_inotify_number(9999999999ULL, (char *)"/proc/sys/fs/inotify/max_user_watches");
    increase_inotify_number(102400UL, (char *)"/proc/sys/fs/inotify/max_queued_events");
    increase_inotify_number(9999999999ULL, (char *)"/proc/sys/fs/inotify/max_user_instances");
    return 0;
}

int init_notify_fs(const char *config_file)
{
    atomic_t index_threads_num;         //the number of initial threads to read file from dir directly.
    atomic_set(&index_threads_num, 0);

    vector<monitor_dir> vdirs;
    vector<string> vstrdirs;
    vdirs.clear();
    vstrdirs.clear();

    g_move_dir = "";
    g_sym_dirs.clear();

    g_events = IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVED_TO /*| IN_DONT_FOLLOW*/;

    g_md = alloc_monitor_dirs();
    if (g_md == NULL)
    {
        debug_sys(LOG_ERR, "failed to alloc monitor_dirs\n");
        return -1;
    }

    if (increase_inotify_watches() != 0)
    {
        debug_sys(LOG_ERR, "increase_inotify_watches failed\n");
        return -1;
    }

    if (!inotifytools_initialize())
    {
        debug_sys(LOG_ERR, "Couldn't initialize inotify\n");
        return -1;
    }

    register_ops();
    create_worker(fs_notify_process, NULL);
    debug_sys(LOG_NOTICE, "Create notify monitor process Successfully.\n");

    create_worker(dir_check_process, NULL);
    debug_sys(LOG_NOTICE, "Create check process Successfully.\n");

    create_worker(dir_change_notify_process, NULL);
    debug_sys(LOG_NOTICE, "Create notify monitor process Successfully.\n");

    if (get_monitor_dir_from_config(config_file, g_md) != 0)
    {
        debug_sys(LOG_ERR, "Couldn't read config file %s\n", config_file);
        return -1;
    }
    debug_sys(LOG_NOTICE, "Read monitor dir info from file %s Successfully.\n", config_file);

    //read dir and update values in memory
    sort_monitor_dirs(g_md, vdirs);
    for (vector<monitor_dir>::iterator it = vdirs.begin(); it != vdirs.end();
         it++)
    {
        vstrdirs.push_back(it->dir_name);
    }
    build_directorys_index(g_md, vstrdirs, MAX_BUILD_THREADS, index_threads_num);
    debug_sys(LOG_NOTICE, "Build directory Successfully.\n");

    //bio thread begins to work now.
    g_build_index_ok = 1;
    debug_sys(LOG_NOTICE, "Watches established, Init notify fs ok!!!\n");

    return 0;
}

