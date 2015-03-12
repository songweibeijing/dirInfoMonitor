INCLUDES = -I$(top_srcdir)/common/include -I../libinotifytools/inc -I../inc
sbin_PROGRAMS = dircounterd
dircounterd_SOURCES = main.cpp util.cpp bio.c log.cpp sig.cpp config.cpp kv.cpp monitor_dir.cpp inotify_process.cpp dump.cpp cJSON.c shm.c readdir.c
dircounterd_CFLAGS = -D_FILE_OFFSET_BITS=64 -D_LARGE_FILE 
dircounterd_CPPFLAGS = -D_FILE_OFFSET_BITS=64 -D_LARGE_FILE -std=gnu++0x 
dircounterd_LDFLAGS = -lpthread -levent -ldb -lpcre -lrt -ldl
dircounterd_LDADD = ../libinotifytools/src/libinotifytools.la 
