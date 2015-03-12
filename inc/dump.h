/*
 * dump.h
 *
 *  Created on: Sep 4, 2014
 *      Author: root
 */

#ifndef DUMP_H_
#define DUMP_H_

#define DATA_FILE_TMP           "/var/log/dircounter_data_tmp"

data_rec test_one_key(char *key, int length);
int dump_thread_create();

#endif /* DUMP_H_ */
