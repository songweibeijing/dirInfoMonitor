#include <sys/types.h>
#include <sys/wait.h>
#include "header.h"
#include "headercxx.h"
#include "sig.h"
#include "monitor_dir.h"
#include "inotify_process.h"
#include "config.h"
#include "util.h"
#include "log.h"
#include "kv.h"
#include "dump.h"

#define DEBUG_KEY_FILE "/usr/local/etc/dc_debug"

extern config g_config;
extern bdb_info *g_hash_db;
extern int g_build_index_ok;

pthread_t sig_handler_thread;

int sig_handler_user1(int signo);
int sig_handler_user2(int signo);
int sig_handler_ttin(int signo);
int sig_handler_ttou(int signo);
int sig_handler_int(int signo);

static struct signal signals[] =
{
    {
        SIGUSR1,
        (char *)"SIGUSR1",
        (char *)"user1",
        0,
        sig_handler_user1
    },

    {
        SIGUSR2,
        (char *)"SIGUSR2",
        (char *)"user2",
        0,
        sig_handler_user2
    },

    {
        SIGTTIN,
        (char *)"SIGTTIN",
        (char *)"up logging level",
        0,
        sig_handler_ttin
    },

    {
        SIGTTOU,
        (char *)"SIGTTOU",
        (char *)"down logging level",
        0,
        sig_handler_ttou
    },

    {
        SIGINT,
        (char *)"SIGINT",
        (char *)"exit",
        0,
        sig_handler_int
    },

    null_signal
};

void signal_handler(int signo);

void *sigmgr_thread(void *arg)
{
    sigset_t waitset;
    siginfo_t info;
    int rc;

    pthread_detach(sig_handler_thread);
    sigemptyset(&waitset);

    struct signal *sig;
    for (sig = signals; sig->signo != 0; sig++)
    {
        sigaddset(&waitset, sig->signo);
    }

    //wait the initial process is ready.
    while (g_build_index_ok == 0)
    {
        //if the start flag is 0, sleep 5s, and try it again.
        my_sleep(1);
    }

    while (1)
    {
        rc = sigwaitinfo(&waitset, &info);
        if (rc != -1)
        {
            printf("sigwaitinfo() fetch the signal - %d\n", rc);
            signal_handler(info.si_signo);
        }
        else
        {
            //printf("sigwaitinfo() returned err: %d; %s\n", errno, strerror(errno));
        }
    }
    return NULL;
}

int signal_init(void)
{
    struct signal *sig;

    //block signal in current thread, unblock signal in thread: sig_handler_thread
    sigset_t bset, oset;
    sigemptyset(&bset);
    for (sig = signals; sig->signo != 0; sig++)
    {
        sigaddset(&bset, sig->signo);
    }

    if (pthread_sigmask(SIG_BLOCK, &bset, &oset) == -1)
    {
        printf("error\n");
        return -1;
    }

    //create thread
    pthread_create(&sig_handler_thread, NULL, sigmgr_thread, NULL);

    return 0;
}

void signal_deinit(void)
{
}

void signal_handler(int signo)
{
    struct signal *sig;
    for (sig = signals; sig->signo != 0; sig++)
    {
        if (sig->signo == signo)
        {
            break;
        }
    }
    assert(sig->signo != 0);

    debug_sys(LOG_NOTICE, "signal %d (%s) received, %s", signo, sig->signame, sig->actionstr);

    if (sig->handler != NULL)
    {
        sig->handler(sig->signo);
    }
}

static void get_all_files(char *dir, vector<string> &vFiles)
{
    vFiles.clear();

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
            continue;
        }
        else if (dp->d_type != DT_DIR)
        {
            vFiles.push_back(string(buf));
        }
    }
}

fileinfo count_dir(char *dir, monitor_dirs *md)
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

    find_update_monitor_dir(md, dir, &fi, ADD);
    return fi;
}

int sig_handler_user1(int signo)
{
    int errors = 0;

    string debug_string;
    map<string , fileinfo> tmpRec;
    map<string , fileinfo> memRec;
    tmpRec.clear();
    memRec.clear();

    vector<monitor_dir> vdirs;
    vdirs.clear();

    monitor_dirs *tmp_md = alloc_monitor_dirs();
    if (tmp_md == NULL)
    {
        debug_sys(LOG_ERR, "failed to alloc monitor_dirs\n");
        return -1;
    }

    get_monitor_dir_from_config(g_config.default_monitor_file, tmp_md);
    sort_monitor_dirs(tmp_md, vdirs);

    debug_sys(LOG_NOTICE, "--------------------\n\n\n\n");
    debug_sys(LOG_NOTICE, "output the count and size by counting directly\n");
    for (vector<monitor_dir>::iterator it = vdirs.begin(); it != vdirs.end();
         it++)
    {
        char dir[1024], tmpstr[1024];
        snprintf(dir, 1024, "%s", (char *)it->dir_name);
        fileinfo fi = count_dir(dir, tmp_md);
        snprintf(tmpstr, 1024, "dir name %s, dirsize %lld, dircount %lld\n", dir, fi.filesz, fi.filenm);
        debug_string += (string)tmpstr;
        tmpRec.insert(make_pair(string(dir), fi));
    }
    debug_sys(LOG_NOTICE, "%s\n", debug_string.c_str());

    free_monitor_dirs(tmp_md);
    tmp_md = NULL;

    debug_sys(LOG_NOTICE, "--------------------\n\n");
    debug_sys(LOG_NOTICE, "output the count and size in memory g_md\n");
    vdirs.clear();
    sort_monitor_dirs(g_md, vdirs);
    print_directory_sort(g_md);
    debug_sys(LOG_NOTICE, "--------------------\n\n");

    debug_string = "";
    debug_sys(LOG_NOTICE, "--------------------\n\n");
    debug_sys(LOG_NOTICE, "output the count and size in shared memory g_md\n");
    vdirs.clear();
    sort_monitor_dirs(g_md, vdirs);
    for (vector<monitor_dir>::iterator it = vdirs.begin(); it != vdirs.end();
         it++)
    {
        char dir[1024], tmpstr[1024];
        snprintf(dir, 1024, "%s", (char *)it->dir_name);
        data_rec drec = test_one_key(dir, strlen(dir));
        snprintf(tmpstr, 1024, "dir name %s, dirsize %lld, dircount %lld\n", dir, drec.fi.filesz, drec.fi.filenm);
        debug_string += (string)tmpstr;
    }
    debug_sys(LOG_NOTICE, "%s", debug_string.c_str());
    debug_sys(LOG_NOTICE, "--------------------\n\n");

    char *special_one = (char *)"/var/spool/postfix/qd/qfolder";
    data_rec drec = test_one_key(special_one, strlen(special_one));
    debug_sys(LOG_NOTICE, "/var/spool/postfix/qd/qfolder info in db, size : %lld, num : %lld\n", drec.fi.filesz, drec.fi.filenm);

    debug_sys(LOG_NOTICE, "Begin to find the missing files--------------------\n\n");
    for (vector<monitor_dir>::iterator it = vdirs.begin(); it != vdirs.end();
         it++)
    {
        memRec.insert(make_pair(string(it->dir_name), it->fi));
    }

    for (map<string, fileinfo>::iterator it = tmpRec.begin();
         it != tmpRec.end(); it++)
    {
        string path = it->first;
        fileinfo directfi = it->second;
        int found = 0;
        for (map<string, fileinfo>::iterator itmap = memRec.begin();
             itmap != memRec.end(); itmap++)
        {
            if (it->first == itmap->first)
            {
                //int type = find_monitor_file_type(g_md, path.c_str());
                found = 1;
                if (it->second.filesz != itmap->second.filesz ||
                    it->second.filenm != itmap->second.filenm)
                {
                    debug_sys(LOG_NOTICE, "path %s, eal sz:%lld, nm:%lld; mem sz:%lld, nm:%lld\n", path.c_str(), it->second.filesz, it->second.filenm, itmap->second.filesz, itmap->second.filenm);
                    errors++;
                    break;
                }

                break;
            }
        }
        if (found == 0)
        {
            errors++;
            debug_sys(LOG_NOTICE, "can not find %s in map\n", path.c_str());
            continue;
        }

    }

    for (map<string, fileinfo>::iterator it = memRec.begin();
         it != memRec.end(); it++)
    {
        string path = it->first;
        fileinfo directfi = it->second;
        int found = 0;
        for (map<string, fileinfo>::iterator itmap = tmpRec.begin();
             itmap != tmpRec.end(); itmap++)
        {
            if (it->first == itmap->first)
            {
                //int type = find_monitor_file_type(g_md, path.c_str());
                if (it->second.filesz != itmap->second.filesz ||
                    it->second.filenm != itmap->second.filenm)
                {
                    debug_sys(LOG_NOTICE, "path %s, real sz:%lld, nm:%lld; mem sz:%lld, nm:%lld\n", path.c_str(), itmap->second.filesz, itmap->second.filenm, it->second.filesz, it->second.filenm);
                    errors++;
                    break;
                }

                found = 1;
                break;
            }
        }
        if (found == 0)
        {
            errors++;
            debug_sys(LOG_NOTICE, "can not find %s in real\n", path.c_str());
            continue;
        }

    }
    debug_sys(LOG_NOTICE, "End to find the missing files, find errors : %d\n", errors);
    return 0;
}

int sig_handler_user2(int signo)
{
    log_fd_invalid = 1;
    return 0;
}

int process_debug_key(char *buf, void *res)
{
    fileinfo value;
    if (get_key_value(g_hash_db, buf, &value) == 1)
    {
        debug_sys(LOG_NOTICE, "path : %s, filenum : %lld, filesize : %lld\n", buf, value.filenm, value.filesz);
    }
    else
    {
        debug_sys(LOG_NOTICE, "can not find value for path : %s\n", buf);
    }
    return 0;
}

int sig_handler_ttin(int signo)
{
    char *file = (char *)DEBUG_KEY_FILE;
    process_file(file, process_debug_key, NULL);
    return 0;
}

int sig_handler_ttou(int signo)
{

    return 0;
}

int sig_handler_int(int signo)
{
    exit(-1);
    return 0;
}

