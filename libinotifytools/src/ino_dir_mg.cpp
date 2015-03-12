#include <string.h>
#include <string>
#include <unordered_map>
#include <iostream>

#include "inotifytools.h"
//#include "ino_dir_mg.h"
#include "pthread.h"

using namespace std;
typedef unordered_map <string, int> strInthashMap;
strInthashMap g_inotify_dirs(10240);
pthread_mutex_t g_inotify_lock = PTHREAD_MUTEX_INITIALIZER;

int __is_dir_added(string dir)
{
    strInthashMap::iterator it = g_inotify_dirs.find(dir);
    if (it != g_inotify_dirs.end())
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

int is_dir_added(char const *dir)
{
    string path(dir, strlen(dir));
    int exist = 0;

    pthread_mutex_lock(&g_inotify_lock);
    exist = __is_dir_added(path);
    pthread_mutex_unlock(&g_inotify_lock);
    printf("is_dir_added %s, exist : %d\n", dir, exist);

    return exist;
}

int add_dir(char *dir)
{
    string path(dir, strlen(dir));
    int exist = 0;

    pthread_mutex_lock(&g_inotify_lock);
    exist = __is_dir_added(path);
    if (exist == 0)
    {
        g_inotify_dirs.insert(make_pair(path, 1));
    }
    pthread_mutex_unlock(&g_inotify_lock);
    printf("try to add_dir %s, exist : %d\n", dir, exist);

    return exist;
}

int del_dir(char *dir)
{
    string path(dir, strlen(dir));

    pthread_mutex_lock(&g_inotify_lock);
    strInthashMap::iterator it = g_inotify_dirs.find(path);
    if (it != g_inotify_dirs.end())
    {
        printf("try to erase %s\n", dir);
        g_inotify_dirs.erase(it);
    }
    pthread_mutex_unlock(&g_inotify_lock);
    return 0;
}

int print_dir()
{
    strInthashMap::iterator it = g_inotify_dirs.begin();
    for (; it != g_inotify_dirs.end(); it++)
    {
        cout << it->first << "\t";
    }
    cout << endl;
    return 0;
}
