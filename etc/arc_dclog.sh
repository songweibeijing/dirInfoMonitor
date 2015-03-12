#!/bin/sh

/usr/sbin/logrotate /usr/local/etc/dcrotated
EXITVALUE=$?
if [ $EXITVALUE != 0 ]; then
	/usr/bin/logger -t logrotate "ALERT exited abnormally with [$EXITVALUE]"
fi
exit 0
	
