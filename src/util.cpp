#include "header.h"
#include "headercxx.h"
#include "util.h"

#define MAX_SENDBUF_SIZE (256 * 1024 * 1024)

void get_timestamp(int level, char *txt)
{

#define STRLEN 256
    time_t gmt;
    struct tm *timeptr;
    struct tm timestruct;

    time(&gmt);
    timeptr = localtime_r(&gmt, &timestruct);
    snprintf(txt, STRLEN,
             "%04d.%02d.%02d %02d:%02d:%02d",
             timeptr->tm_year + 1900, timeptr->tm_mon + 1, timeptr->tm_mday,
             timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec);
}

int my_write(int fd, char *buffer, int size)
{
    while (1)
    {
        if (write(fd, buffer, size) < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }
            else
            {
                return -1;
            }
        }
        else
        {
            return 0;
        }
    }
    return 0;
}

int file_size(char *path)
{
    struct stat buf;
    int ret = stat(path, &buf);
    if (ret < 0)
    {
        return -1;
    }
    return buf.st_size;
}

int file_exist(char *path)
{
    if (access(path, F_OK) == 0)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

int file_exist_ex(const char *path, mode_t iFileType)
{
    struct stat f_stat;

    if (stat(path, &f_stat) == -1)
    {
        return 0;
    }

    if (!(f_stat.st_mode & iFileType))
    {
        return 0;
    }

    return 1;
}

void my_sleep(int sec)
{
    struct timeval interval;
    interval.tv_sec = sec;
    interval.tv_usec = 0;
    select(0, NULL, NULL, NULL, &interval);
}

void my_usleep(int usec)
{
    struct timeval interval;
    interval.tv_sec = 0;
    interval.tv_usec = usec;
    select(0, NULL, NULL, NULL, &interval);
}

//return 1 means ok.
int make_dir(const char *path, mode_t iFlag)
{
    int iRetCode = mkdir(path, iFlag);
    if (iRetCode < 0 && errno == EEXIST)
    {
        return file_exist_ex(path, S_IFDIR);
    }

    return iRetCode == 0 ? 1 : 0;
}

//return 1 means ok.
int make_dir_recusive(char *path, mode_t iFlag)
{
    char *begin = NULL;
    char *end = NULL;
    char *pos = NULL;
    char *save_path = NULL;
    int ret = 1;

    if (path == NULL || strlen(path) == 0)
    {
        return -1;
    }

    save_path = strdup(path);
    if (save_path == NULL)
    {
        return -1;
    }

    begin = save_path;
    end = save_path + strlen(save_path);
    if (*begin != '/')
    {
        my_free(save_path);
        return -1;
    }
    begin++;

    while (begin < end)
    {
        pos = strchr(begin, '/');
        if (pos == NULL)
        {
            ret = make_dir(save_path, iFlag);
            break;
        }
        else
        {
            *pos = '\0';
            begin = pos + 1;
            if (!make_dir(save_path, iFlag))
            {
                ret = -1;
                break;
            }
            *pos = '/';
        }
    }

    my_free(save_path);
    return ret;
}

int delstr(char *str, const char *delchs)
{
    int size = strlen(str);
    int span = 0;
    for (int i = 0; i < size;)
    {
        if (strchr(delchs, str[i]) != NULL)
        {
            span++;
            i++;
            continue;
        }
        else if (span > 0)
        {
            str[i - span] = str[i];
            i++;
        }
        else
        {
            i++;
            continue;
        }
    }
    for (int i = size - span; i < size; i++)
    {
        str[i] = '\0';
    }
    return size - span;
}

int splitstr(char *str, const char *splits)
{
    assert(str != NULL);
    assert(splits != NULL);
    char *p = str;
    while (p && *p)
    {
        if (strchr(splits, *p) != NULL)
        {
            *p = '\0';
            return (int)(p - str);
        }
        p++;
    }
    return (int)(p - str);
}

int get_parent_dir(char *buf, char *parent)
{
    char *end = buf + strlen(buf) - 1;
    if (*end == '/' && strlen(buf) > 1)
    {
        *end = '\0';
    }
    char *sep = strrchr(buf, '/');
    if (sep == NULL)
    {
        printf("path :%s has no parent directory\n", buf);
        return -1;
    }
    if (sep == buf)
    {
        strncpy(parent, buf, 1);
    }
    else
    {
        strncpy(parent, buf, sep - buf);
    }
    return 0;
}

int get_all_parent_dir(char *path, vector<string> &vDirs)
{
    char tmp[MAX_PATH] = {0};
    vDirs.clear();

    if (strlen(path) == 1 && *path == '/')
    {
        return 0;
    }

    snprintf(tmp, MAX_PATH, "%s", path);
    while (1)
    {
        char parent[MAX_PATH] = {0};
        get_parent_dir(tmp, parent);
        vDirs.push_back(parent);
        if (strlen(parent) == 1 && parent[0] == '/')
        {
            break;
        }
        snprintf(tmp, MAX_PATH, "%s", parent);
    }
    return 0;
}

int get_all_subdir(char *dir, int level, vector<path_level> &vRets)
{
    char buf[4096] = {0};
    DIR *top_defer_dir = NULL;
    struct dirent *dp = NULL;
    vector<path_level> vtmp;
    vtmp.clear();

    if (level < 1)
    {
        return 0;
    }

    path_level pl;
    pl.level = level;
    pl.path = string(dir, strlen(dir));
    vRets.push_back(pl);
    if (level == 1)
    {
        return 0;
    }

    level--;
    top_defer_dir = opendir(dir);
    while (top_defer_dir)
    {
        dp = (struct dirent *)readdir64(top_defer_dir);
        if (dp == NULL)
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
            pl.level = level;
            pl.path = string(buf, strlen(buf));
            vtmp.push_back(pl);
        }
        else
        {
            struct stat64 my_stat;
            if (lstat64(buf, &my_stat) == 0)
            {
                if (S_ISDIR(my_stat.st_mode))
                {
                    pl.level = level;
                    pl.path = string(buf, strlen(buf));
                    vtmp.push_back(pl);
                }
                if (S_ISLNK(my_stat.st_mode))
                {
                    struct stat64 my_stat2;
                    if (stat64(buf, &my_stat2) == 0 && S_ISDIR(my_stat2.st_mode))
                    {
                        pl.level = level;
                        pl.path = string(buf, strlen(buf));
                        vtmp.push_back(pl);
                    }
                }
            }
        }
    }

    for (vector<path_level>::iterator it = vtmp.begin();
         it != vtmp.end(); it++)
    {
        char path[256] = {0};
        memcpy(path, it->path.c_str(), it->path.length());
        get_all_subdir(path, level, vRets);
    }

    return 0;
}

int get_tmpfile(char *dir, char *file)
{
    int fd = -1;
    int i = 0;
    char tmp[64] = {0};
    char path[MAX_PATH] = {0};
    char dir_path[128] = {0};

    time_t t = time(NULL);
    strftime(tmp, sizeof(tmp), "%Y%m%d%H", localtime(&t));
    snprintf(dir_path, sizeof(dir_path), "%s%s/", dir, tmp);

    for (i = 0; i < 2; i++)
    {
        snprintf(path, sizeof(path), "%sXXXXXX", dir_path);
        fd = mkostemp(path, O_APPEND | O_SYNC);
        if (fd < 0)
        {
            mkdir(dir_path, 0777);
        }
        else
        {
            break;
        }
    }

    if (fd < 0)
    {
        return -1;
    }

    snprintf(file, MAX_PATH, "%s", path);
    return fd;
}

int process_file(const char *file, cfg_handle handle, void *cfg)
{
    FILE *fp = fopen(file, "rb");
    if (fp == NULL)
    {
        return -1;
    }
    char buf[TMP_BUFSIZ] = {0};
    while (fgets(buf, TMP_BUFSIZ, fp) != NULL)
    {
        if (strlen(buf) == 0)
        {
            continue;
        }
        if (buf[strlen(buf) - 1] == '\n')
        {
            buf[strlen(buf) - 1] = '\0';
        }
        if (buf[strlen(buf) - 1] == '\r')
        {
            buf[strlen(buf) - 1] = '\0';
        }
        if (delstr(buf, " \t") == 0)
        {
            continue;
        }
        if (splitstr(buf, "#") == 0)
        {
            continue;
        }
        if (handle(buf, cfg) != 0)
        {
            cerr << __FILE__ << " handle : " << buf << " error" << endl;
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

int is_key_set(strCharhashMap &maps, string key)
{
    int is_found = NFOUND;
    strCharhashMap::iterator it = maps.find(key);
    if (it != maps.end())
    {
        is_found = FOUND;
    }
    return is_found;
}

//if key exists, return 1.
int add_key_set(strCharhashMap &maps, string key)
{
    strCharhashMap::iterator it = maps.find(key);
    if (it != maps.end())
    {
        return FOUND;
    }

    maps.insert(make_pair(key, 1));
    return NFOUND;
}

//if key exists, return 1.
int del_key_set(strCharhashMap &maps, string key)
{
    strCharhashMap::iterator it = maps.find(key);
    if (it != maps.end())
    {
        maps.erase(it);
        return FOUND;
    }

    return NFOUND;
}

