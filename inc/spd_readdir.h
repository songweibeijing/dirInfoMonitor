#ifndef _SPD_READDIR_H
#define _SPD_READDIR_H

#include <sys/types.h>
#include <dirent.h>

#define opendir     spd_opendir
#define fdopendir   spd_fdopendir
#define closedir    spd_closedir
#define readdir     spd_readdir
#define readdir64   spd_readdir64
#define readdir_r   spd_readdir_r
#define readdir64_r spd_readdir64_r
#define telldir     spd_telldir
#define seekdir     spd_seekdir
#define dirfd       spd_dirfd

/*
 * we should only use below spd_* functions.
 * other functions will get compiler warning.
 */
DIR *opendir(const char *name);
int closedir(DIR *dir);
struct dirent *readdir(DIR *dir);
struct dirent64 *readdir64(DIR *dir);

#endif
