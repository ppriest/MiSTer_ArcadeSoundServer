#!/bin/sh
# arcsnd: arcade sound boards on the ARM (Konami Hornet, Hyper NeoGeo 64), in the background. It
# waits for a set it knows, goes quiet at every core load, and runs as one copy.
#   (no argument)  from the Scripts menu: stop it if it is running, else start it
#   start          start it if it is not running (linux/user-startup.sh's $1 at boot)
#   stop           stop it
DIR=/media/fat/games/arcsnd
LOG=/tmp/arcsnd.log
running() { pidof arcsnd >/dev/null; }
launch() { cd "$DIR" || exit 1; chmod +x arcsnd; setsid ./arcsnd </dev/null >/dev/null 2>"$LOG" & }
halt() { kill $(pidof arcsnd); sleep 1; if running; then echo "arcsnd did not stop"; exit 1; fi; echo "arcsnd stopped"; }
case "$1" in
start) running || launch ;;
stop) if running; then halt; fi ;;
*)
	if running; then echo "arcsnd is running: stopping it"; halt; exit 0; fi
	echo "starting arcsnd"; launch; sleep 3
	if running; then echo "arcsnd started:"; head -n 20 "$LOG"; else echo "arcsnd failed to start:"; tail -n 20 "$LOG"; exit 1; fi
	;;
esac
