#!/bin/sh
#
# Removed dependency from /etc/rc.

cleartmp_prestart()
{
	local x11_socket_dirs="/tmp/.X11-unix /tmp/.ICE-unix /tmp/.font-unix \
	    /tmp/.XIM-unix"

	# Remove X lock files, since they will prevent you from restarting X.
	rm -f /tmp/.X[0-9]-lock

	# Create socket directories with correct permissions to avoid
	# security problem.
	#
	rm -fr ${x11_socket_dirs}
	mkdir -m 1777 ${x11_socket_dirs}
}

cleartmp_start()
{
	echo "Clearing /tmp."
	#
	#	Prune quickly with one rm, then use find to clean up
	#	/tmp/[lq]* (this is not needed with mfs /tmp, but
	#	doesn't hurt anything).
	#
	(cd /tmp && rm -rf [a-km-pr-zA-Z]* &&
	    find -x . ! -name . ! -name lost+found ! -name quota.user \
		! -name quota.group ! -name .X11-unix ! -name .ICE-unix \
		! -name .font-unix ! -name .XIM-unix \
		-exec rm -rf -- {} \; -type d -prune)
}

# start here
# used to emulate "requires/provide" functionality
pidfile="/var/run/cleanvar.pid"
touch $pidfile

cleartmp_prestart
cleartmp_start
exit 0
