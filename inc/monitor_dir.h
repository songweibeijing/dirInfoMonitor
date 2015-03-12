#ifndef _MONITOR_DIR_H
#define _MONITOR_DIR_H

#include "header.h"
#include "headercxx.h"
#include "kv.h"
#include <pcre.h>
#include <string>
#include <unordered_map>

using namespace std;

#define MAX_MONITOR_DIRS 4096

typedef unordered_map <string, char> strCharhashMap;
typedef unordered_map <string, int> strInthashMap;
typedef unordered_map <string, vector<string> > strVectorhashMap;

typedef struct exclude_dir
{
    string dirpattern;
    pcre *re;
} edir;

typedef struct exclude_dir_array
{
    vector<edir> vdirs;
    strCharhashMap ex_dir_map;
    strVectorhashMap sub_dir_map;
    pthread_mutex_t ex_lock;
} exclude_dir_array;

typedef struct monitor_dir
{
    char dir_name[MAX_PATH];
    uint32_t file_status; //1means the directory will added to inotify mechanism.
    /**the level of the directory, 1 mean this directory only contains files.
       if you just want to monitor the files in one dir, its value is 1.
       if you want to monitor all files in one dir and all files in the first-level subdirs of one dir, its value is 2.
    **/
    uint8_t directory_level;
    uint8_t is_counter_size; //optimize for speeding up
    fileinfo fi;
} monitor_dir, *p_monitor_dir;
typedef unordered_map <string, p_monitor_dir> strhashMap;

typedef struct monitor_dirs
{
    strhashMap md;
    pthread_rwlock_t md_lock;
    int md_mum;
    exclude_dir_array ex_dirs;
} monitor_dirs;

extern monitor_dirs *g_md;
extern int g_events;
extern pthread_mutex_t g_delete_dir_lock;
extern strCharhashMap g_delete_dir;

//for exclude dir
int add_exclude_pattern(char *dirpattern , exclude_dir_array *ed);
int is_exclude_dir(char *dir , exclude_dir_array *ed);

int add_sub_exclude_dir(char *dir, char *subdir, exclude_dir_array *ed);
int get_sub_exclude_dir(char *dir, exclude_dir_array *ed, vector<string> &vSubdirs);

//for monitor dir
monitor_dirs *alloc_monitor_dirs();
void free_monitor_dirs(monitor_dirs *md);

int __find_monitor_dir(monitor_dirs *md, char *path, monitor_dir **target);
int find_monitor_dir(monitor_dirs *md, char *path, monitor_dir *target);
int find_update_monitor_dir(monitor_dirs *md, char *path, fileinfo *delta, int type);
int find_monitor_file_type(monitor_dirs *md, const char *path);
int find_monitor_file_level(monitor_dirs *md, const char *path, int level);

int del_monitor_dir(monitor_dirs *md, char *path);
int add_monitor_dir(monitor_dirs *md, char *path, int &level, int is_counter_size);

int del_monitor_dir_inotify(monitor_dirs *md, char *path);
int add_monitor_dir_inotify(monitor_dirs *md, char *path, int level, int is_counter_size, int f);

int del_dir_inotify(char *path);
int add_dir_inotify(monitor_dirs *md, char *path, int level);

void print_directory_sort(monitor_dirs *md);

int get_all_parent_dir(char *path, vector<string> &vDirs);
void convert_to_param(monitor_dirs *md, vector<txn_param> &params);
int sort_monitor_dirs(monitor_dirs *md, vector<monitor_dir> &vDirs);
void get_delete_keys(monitor_dirs *md, vector<string> &dbkeys, vector<string> &deletekeys);


#endif
