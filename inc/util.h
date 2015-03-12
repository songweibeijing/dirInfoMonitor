#ifndef _UTIL_H
#define _UTIL_H

#include "header.h"
#include "headercxx.h"
#include <vector>
#include <string>
#include <unordered_map>

using namespace std;

typedef unordered_map <string, char> strCharhashMap;

void get_timestamp(int level, char *txt);
int my_write(int fd, char *buffer, int size);
int file_exist(char *path);
int file_size(char *path);
int get_parent_dir(char *buf, char *parent);
int get_tmpfile(char *dir, char *file);
void my_sleep(int sec);
void my_usleep(int usec);
int make_dir(const char *path, mode_t iFlag);
int make_dir_recusive(char *path, mode_t iFlag);
int delstr(char *str, const char *delchs);
int splitstr(char *str, const char *splits);

typedef int (*cfg_handle)(char *buf, void *res);
int process_file(const char *file, cfg_handle handle, void *cfg);

int get_all_parent_dir(char *path, vector<string> &vDirs);
int get_all_subdir(char *dir, int level, vector<path_level> &vRets);

int is_key_set(strCharhashMap &maps, string key);
//if key exists, return 1.
int add_key_set(strCharhashMap &maps, string key);
//if key exists, return 1.
int del_key_set(strCharhashMap &maps, string key);


#endif
