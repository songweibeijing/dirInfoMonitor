#include <algorithm>
#include "inotifytools.h"
#include "inotify.h"
#include "inotify-nosys.h"
#include "inotifytools_p.h"
#include "headercxx.h"
#include "util.h"
#include "monitor_dir.h"
#include "inotify_process.h"
#include "kv.h"
#include "log.h"

monitor_dirs *g_md = NULL;
pthread_mutex_t g_delete_dir_lock = PTHREAD_MUTEX_INITIALIZER;
strCharhashMap g_delete_dir(1024);

static int __find_exclude_pattern(char *pattern, exclude_dir_array *ed)
{
    string strpatn(pattern, strlen(pattern));
    vector<edir> vdirs = ed->vdirs;
    for (vector<edir>::iterator it = vdirs.begin();
         it != vdirs.end(); it++)
    {
        if (it->dirpattern == strpatn)
        {
            return FOUND;
        }
    }
    return NFOUND;
}

int add_exclude_pattern(char *dirpattern , exclude_dir_array *ed)
{
    const char *error = NULL;
    int erroffset = 0;
    pcre *re = NULL;
    edir dir_tmp;

    pthread_mutex_lock(&ed->ex_lock);
    if (__find_exclude_pattern(dirpattern, ed) == FOUND)
    {
        pthread_mutex_unlock(&ed->ex_lock);
        return FOUND;
    }

    re = pcre_compile(dirpattern, 0, &error, &erroffset, NULL);
    if (re == NULL)
    {
        pthread_mutex_unlock(&ed->ex_lock);
        debug_sys(LOG_ERR, "PCRE compilation telephone failed at offset %d: %s\n", erroffset,  error);
        return ERROR;
    }

    dir_tmp.dirpattern = string(dirpattern, strlen(dirpattern));
    dir_tmp.re = re;
    ed->vdirs.push_back(dir_tmp);
    pthread_mutex_unlock(&ed->ex_lock);

    return NFOUND;
}

static int __is_exclude_dir(char *dir , exclude_dir_array *ed)
{
    int found = NFOUND;
    for (vector<edir>::iterator it = ed->vdirs.begin();
         it != ed->vdirs.end(); it++)
    {
        edir tmped = *it;
        int ovector[10];
        int matched = pcre_exec(tmped.re, NULL, dir, strlen(dir), 0, 0, ovector, sizeof(ovector) / sizeof(int));
        if (matched > 0)
        {
            found = FOUND;
        }
    }

    return found;
}

int is_exclude_dir(char *dir , exclude_dir_array *ed)
{
    int found = NFOUND;
    strInthashMap::iterator itmap;
    string dirstr = string(dir, strlen(dir));

    pthread_mutex_lock(&ed->ex_lock);
    found = is_key_set(ed->ex_dir_map, dirstr);
    if (found == NFOUND)
    {
        found = __is_exclude_dir(dir, ed);
        if (found == FOUND)
        {
            add_key_set(ed->ex_dir_map, dirstr);
        }
    }
    pthread_mutex_unlock(&ed->ex_lock);
    return found;
}

int add_sub_exclude_dir(char *dir, char *subdir, exclude_dir_array *ed)
{
    int found = NFOUND;
    string strdir = string(dir, strlen(dir));
    vector<string> vSubs;
    vSubs.clear();

    pthread_mutex_lock(&ed->ex_lock);
    strVectorhashMap::iterator it = ed->sub_dir_map.find(strdir);
    if (it == ed->sub_dir_map.end())
    {
        vector<string> vtmp;
        vtmp.clear();
        vtmp.push_back(string(subdir));
        ed->sub_dir_map.insert(make_pair(strdir, vtmp));
    }
    else
    {
        vSubs = it->second;
        for (vector<string>::iterator it_sub = vSubs.begin();
             it_sub != vSubs.end(); it_sub++)
        {
            if (*it_sub == string(subdir))
            {
                found = FOUND;
                break;
            }
        }
        if (found == NFOUND)
        {
            it->second.push_back(string(subdir));
        }
    }
    pthread_mutex_unlock(&ed->ex_lock);
    return 0;
}

int get_sub_exclude_dir(char *dir, exclude_dir_array *ed, vector<string> &vSubdirs)
{
    int found = NFOUND;
    string strdir = string(dir, strlen(dir));
    pthread_mutex_lock(&ed->ex_lock);
    strVectorhashMap::iterator it = ed->sub_dir_map.find(strdir);
    if (it != ed->sub_dir_map.end())
    {
        found = FOUND;
        vSubdirs = it->second;
    }
    pthread_mutex_unlock(&ed->ex_lock);
    return found;
}

monitor_dirs *alloc_monitor_dirs()
{
    monitor_dirs *md = new monitor_dirs;
    if (md == NULL)
    {
        return NULL;
    }
    md->md.clear();
    md->md_mum = 0;
    pthread_rwlock_init(&md->md_lock, NULL);

    exclude_dir_array *p_ex_dir = &md->ex_dirs;
    p_ex_dir->vdirs.clear();
    p_ex_dir->ex_dir_map.clear();
    p_ex_dir->sub_dir_map.clear();
    pthread_mutex_init(&p_ex_dir->ex_lock, NULL);

    return md;
}

void free_monitor_dirs(monitor_dirs *md)
{
    if (!md)
    {
        return;
    }

    md->md.clear();
    pthread_rwlock_destroy(&md->md_lock);

    exclude_dir_array *p_ex_dir = &md->ex_dirs;
    for (vector<edir>::iterator it = p_ex_dir->vdirs.begin();
         it != p_ex_dir->vdirs.end(); it++)
    {
        edir tmped = *it;
        my_free(tmped.re);
    }

    p_ex_dir->vdirs.clear();
    p_ex_dir->ex_dir_map.clear();

    pthread_mutex_destroy(&p_ex_dir->ex_lock);

    delete md;
    md = NULL;
}

int __find_monitor_dir(monitor_dirs *md, char *path, monitor_dir **target)
{
    string strTmp(path, strlen(path));
    strhashMap::iterator it = md->md.find(strTmp);
    if (it == md->md.end())
    {
        *target = NULL;
        return NFOUND;
    }

    *target = it->second;
    if (*target == NULL)
    {
        md->md.erase(it);
        return NFOUND;
    }
    return FOUND;
}

int find_monitor_dir(monitor_dirs *md, char *path, monitor_dir *target)
{
    int ret = NFOUND;
    monitor_dir *tmp = NULL;

    pthread_rwlock_rdlock(&md->md_lock);
    ret = __find_monitor_dir(md, path, &tmp);
    if (ret == FOUND)
    {
        memcpy(target, tmp, sizeof(monitor_dir));
    }
    pthread_rwlock_unlock(&md->md_lock);
    return ret;
}

static void update_fileinfo(fileinfo *target, fileinfo *src, int type)
{
    if (type == ADD)
    {
        target->filenm += src->filenm;
        target->filesz += src->filesz;
    }
    else if (type == DEL)
    {
        target->filenm -= src->filenm;
        target->filesz -= src->filesz;
    }
}

int find_update_monitor_dir(monitor_dirs *md, char *path, fileinfo *delta, int type)
{
    int ret = NFOUND;
    monitor_dir *tmp = NULL;

    pthread_rwlock_wrlock(&md->md_lock);
    ret = __find_monitor_dir(md, path, &tmp);
    if (ret == FOUND)
    {
        update_fileinfo(&tmp->fi, delta, type);
    }
    pthread_rwlock_unlock(&md->md_lock);
    return ret;
}

static int __find_monitor_file_level(monitor_dirs *md, const char *path, int level)
{
    vector<string> vDirs;
    monitor_dir *tmp = NULL;
    vDirs.clear();
    int newlevel = 1;

    if (level > 0)
    {
        return level;
    }

    get_all_parent_dir((char *)path, vDirs);
    for (vector<string>::iterator it = vDirs.begin();
         it != vDirs.end(); it++)
    {
        if (__find_monitor_dir(md, (char *)(*it).c_str(), &tmp) == FOUND)
        {
            newlevel = tmp->directory_level - 1; //decrease the level.
            break;
        }
    }

    return newlevel;
}

int find_monitor_file_level(monitor_dirs *md, const char *path, int level)
{
    int nlevel = 0;

    pthread_rwlock_rdlock(&md->md_lock);
    nlevel = __find_monitor_file_level(md, path, level);
    pthread_rwlock_unlock(&md->md_lock);

    return nlevel;
}

int find_monitor_file_type(monitor_dirs *md, const char *path)
{
    int type = 0, found = NFOUND;
    string save_path(path, strlen(path));
    monitor_dir dirobj;
    vector<string> vAllParents;

    memset(&dirobj, 0, sizeof(dirobj));
    vAllParents.clear();

    if ((NULL == md) || (NULL == path) || (0 == strlen(path)))
    {
        debug_sys(LOG_ERR, "illegal parameter!\n");
        return 0;
    }

    get_all_parent_dir((char *)save_path.c_str(), vAllParents);
    for (vector<string>::iterator it = vAllParents.begin();
         it != vAllParents.end(); it++)
    {
        if (find_monitor_dir(md, (char *)it->c_str(), &dirobj) == FOUND)
        {
            debug_sys(LOG_DEBUG, "path:%s, parent path with type:%s\n", save_path.c_str(), (char *)it->c_str());
            type = dirobj.is_counter_size;
            found = FOUND;
            break;
        }
    }

    if (found == NFOUND)
    {
        debug_sys(LOG_ERR, "Failed to find the parent for %s!, use the default value 0\n", path);
    }
    return type;
}

int del_monitor_dir(monitor_dirs *md, char *path)
{
    monitor_dir *old = NULL;
    string strTmp(path, strlen(path));

    pthread_rwlock_wrlock(&md->md_lock);
    strhashMap::iterator it = md->md.find(strTmp);
    if (it == md->md.end())
    {
        pthread_rwlock_unlock(&md->md_lock);
        return ERROR;
    }
    old = (monitor_dir *)it->second;
    md->md.erase(it);
    md->md_mum--;
    pthread_rwlock_unlock(&md->md_lock);

    my_free(old);
    return SUCC;
}

int add_monitor_dir(monitor_dirs *md, char *path, int &level, int is_counter_size)
{
    monitor_dir *tmp = NULL;
    monitor_dir *newone = NULL;

    debug_sys(LOG_NOTICE, "begin to add_monitor_dir for %s\n", path);

    pthread_rwlock_wrlock(&md->md_lock);
    if (__find_monitor_dir(md, path, &tmp) == FOUND)
    {
        debug_sys(LOG_NOTICE, "path exist, skip add monitor %s\n", path);
        pthread_rwlock_unlock(&md->md_lock);
        return FOUND;
    }

    newone = (monitor_dir *)calloc(1, sizeof(monitor_dir));
    if (newone == NULL)
    {
        debug_sys(LOG_ERR, "Allocate memory failed for %s\n", path);
        pthread_rwlock_unlock(&md->md_lock);
        return ERROR;
    }

    snprintf(newone->dir_name, MAX_PATH, "%s", path);
    newone->directory_level = __find_monitor_file_level(md, path, level);
    newone->file_status = 1;
    newone->is_counter_size = is_counter_size;
    md->md.insert(make_pair(string(path, strlen(path)), newone));
    md->md_mum++;
    pthread_rwlock_unlock(&md->md_lock);

    level = newone->directory_level;

    return SUCC;
}

void print_directory_sort(monitor_dirs *md)
{
    string debug_string;
    vector<monitor_dir> vDirs;
    vDirs.clear();
    sort_monitor_dirs(md, vDirs);
    for (vector<monitor_dir>::iterator it = vDirs.begin();
         it != vDirs.end(); it++)
    {
        char tmpstr[1024] = {0};
        monitor_dir dirinfo = *it;
        snprintf(tmpstr, 1024, "dir name %s, status %d, dirsize %lld, dircount %lld\n",
                 dirinfo.dir_name, dirinfo.file_status, dirinfo.fi.filesz, dirinfo.fi.filenm);
        debug_string += string(tmpstr);
    }
    debug_sys(LOG_NOTICE, "%s\n", debug_string.c_str());
}

int del_monitor_dir_inotify(monitor_dirs *md, char *path)
{
    debug_sys(LOG_NOTICE, "Delete dir inotify for %s\n", path);
    /*
        if (file_exist(path))
        {
            debug_sys(LOG_ERR, "Path %s should not exist\n", path);
            return ERROR;
        }
    */
    del_monitor_dir(md, path);
    inotifytools_remove_watch_by_filename(path);
    return SUCC;
}

int del_dir_inotify(char *path)
{
    inotifytools_remove_filename_prefix(path);
    return SUCC;
}

int add_dir_inotify(monitor_dirs *md, char *path, int level)
{
    char **exclude_dirs = NULL;
    int ret, size = 0;
    vector<string> vSubdirs;
    vSubdirs.clear();
    string debug_string = "";

    get_sub_exclude_dir(path, &md->ex_dirs, vSubdirs);

    debug_sys(LOG_DEBUG, "add intotify for dir %s\n", path);
    size = vSubdirs.size();
    if (size > 0)
    {
        exclude_dirs = (char **)calloc(size + 1, sizeof(char *));
        if (exclude_dirs == NULL)
        {
            debug_sys(LOG_ERR, "malloc failed for %s\n", path);
            return -1;
        }
        for (int i = 0; i < size; i++)
        {
            exclude_dirs[i] = strdup((char *)vSubdirs[i].c_str());
        }
    }
    ret = add_notify_dir(path, g_events, level + 1, exclude_dirs);

    if (exclude_dirs != NULL)
    {
        for (int i = 0; i < size; i++)
        {
            char *p = exclude_dirs[i];
            my_free(p);
        }
        my_free(exclude_dirs);
    }
    return SUCC;
}

int add_monitor_dir_inotify(monitor_dirs *md, char *path, int level, int is_counter_size, int f)
{
    debug_sys(LOG_NOTICE, "Add dir inotify for %s\n", path);

    if (!file_exist(path))
    {
        debug_sys(LOG_ERR, "file %s not exist\n", path);
        return ERROR;
    }

    if (add_monitor_dir(md, path, level, is_counter_size) == SUCC || f == 1)
    {
        return add_dir_inotify(md, path, level);
    }
    return SUCC;
}

static bool cmp_string_length(const monitor_dir &v1, const monitor_dir &v2)
{
    return strlen(v1.dir_name) > strlen(v2.dir_name);//longest path first.
}

int sort_monitor_dirs(monitor_dirs *md, vector<monitor_dir> &vDirs)
{
    vDirs.clear();
    pthread_rwlock_rdlock(&md->md_lock);
    for (strhashMap::iterator it = md->md.begin();
         it != md->md.end(); it++)
    {
        vDirs.push_back(*it->second);
    }
    pthread_rwlock_unlock(&md->md_lock);

    if (vDirs.size() > 0)
    {
        sort(vDirs.begin(), vDirs.end(), cmp_string_length);
    }
    return SUCC;
}

//if the dbkeys do not exist in md, add it into deletekeys list.
void get_delete_keys(monitor_dirs *md, vector<string> &dbkeys, vector<string> &deletekeys)
{
    monitor_dir *target = NULL;
    for (vector<string>::iterator it = dbkeys.begin();
         it != dbkeys.end(); it++)
    {
        int found = NFOUND;
        pthread_rwlock_rdlock(&md->md_lock);
        found = __find_monitor_dir(md, (char *)(*it).c_str(), &target);
        pthread_rwlock_unlock(&md->md_lock);

        if (found == NFOUND)
        {
            deletekeys.push_back(*it);
        }
    }
}

void convert_to_param(monitor_dirs *md, vector<txn_param> &params)
{
    txn_param param;
    params.clear();

    //find the deleted dir, and clear it after use.
    pthread_mutex_lock(&g_delete_dir_lock);
    for (strCharhashMap::iterator it = g_delete_dir.begin(); it != g_delete_dir.end();)
    {
        string path = it->first;
        memset(&param, 0, sizeof(txn_param));
        param.should_free = true;
        param.type = DELETE;
        param.key = strdup(path.c_str());
        param.keysize = path.length();
        param.value = calloc(1, sizeof(fileinfo)); //this code is meanless, just for add_tnx_param.
        param.valuesize = sizeof(fileinfo);
        add_txn_param(param, params);
        g_delete_dir.erase(it++);
    }
    pthread_mutex_unlock(&g_delete_dir_lock);

    //convert the value in g_md.
    pthread_rwlock_rdlock(&md->md_lock);
    for (strhashMap::iterator it = md->md.begin();
         it != md->md.end(); it++)
    {
        monitor_dir *target = it->second;

        memset(&param, 0, sizeof(txn_param));
        param.should_free = true;
        param.type = INSERT;
        param.key = strdup(target->dir_name);
        param.keysize = strlen(target->dir_name);
        param.value = calloc(1, sizeof(target->fi));
        if (param.value)
        {
            memcpy(param.value, &target->fi, sizeof(target->fi));
        }
        else
        {
            continue;
        }
        param.valuesize = sizeof(target->fi);
        add_txn_param(param, params);
    }
    pthread_rwlock_unlock(&md->md_lock);
}

