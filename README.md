dircounter
==========
This server will count the number of files and total size of specific folders, it uses berkeley db to store the temp data if the memory usage outweighes the maximum value, and use linux inotify mechanism to get the files change information.
