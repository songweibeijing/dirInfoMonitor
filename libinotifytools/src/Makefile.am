INCLUDES = -I../inc/
lib_LTLIBRARIES = libinotifytools.la
libinotifytools_la_SOURCES = inotifytools.c inotifytools_p.h redblack.c redblack.h ino_dir_mg.cpp
libinotifytools_la_LDFLAGS = -version-info 4:1:4 -lpthread
libinotifytools_la_CLAGS = -D_FILE_OFFSET_BITS=64 -D_LARGE_FILE
libinotifytools_la_CPPFLAGS = -D_FILE_OFFSET_BITS=64 -D_LARGE_FILE -std=gnu++0x 
