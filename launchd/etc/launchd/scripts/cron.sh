#!/bin/sh

launch_xml=/sbin/launch_xml
sed=/usr/bin/sed
tmpfile=/tmp/.launchd.crontab
cron="/sbin/nodaemon /usr/sbin/cron /var/run/cron.pid"
pidfile=/var/run/cron.pid
mark='GENERATED BY LAUNCHD DO NOT EDIT FROM THIS LINE ON'

touch $pidfile

numjobs=`$launch_xml -num pfsense.cron.item.command`

$sed -n "1,/$mark/p" /etc/crontab | $sed -e "/$mark/d" > $tmpfile
echo "# $mark" >> $tmpfile

for i in `/bin/seq 1 $numjobs`
do
	command=`$launch_xml -get pfsense.cron.item.command-$i`
	who=`$launch_xml -get pfsense.cron.item.who-$i`
	minute=`$launch_xml -get pfsense.cron.item.minute-$i`
	hour=`$launch_xml -get pfsense.cron.item.hour-$i`
	mday=`$launch_xml -get pfsense.cron.item.mday-$i`
	wday=`$launch_xml -get pfsense.cron.item.mday-$i`
	month=`$launch_xml -get pfsense.cron.item.month-$i`

	echo "$minute $hour $mday $month $wday $who $command" >> $tmpfile
done

mv $tmpfile /etc/crontab
exec $cron 