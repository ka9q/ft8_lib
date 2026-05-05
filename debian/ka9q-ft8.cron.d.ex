#
# Regular cron jobs for the ka9q-ft8 package.
#
0 4	* * *	root	[ -x /usr/bin/ka9q-ft8_maintenance ] && /usr/bin/ka9q-ft8_maintenance
