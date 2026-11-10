#!/system/bin/sh
# late_start service: wait for boot, then run the injector daemon under a watchdog.
#
# The daemon owns its pidfile now (it writes it under the single-instance
# lock), so this script never writes one — a losing watchdog spawn would
# otherwise overwrite the live daemon's pid with its own dead child.

MODDIR=${0%/*}

LOG=/data/adb/codm/injector.log
PIDFILE=/data/adb/codm/injector.pid

mkdir -p /data/adb/codm
chmod 700 /data/adb/codm

while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 3
done

[ -x "$MODDIR/bin/codm-inject" ] || exit 0

(
    while true; do
        if [ -f "$PIDFILE" ]; then
            p=$(cat "$PIDFILE" 2>/dev/null)
            if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
                sleep 10
                continue
            fi
        fi
        "$MODDIR/bin/codm-inject" >> "$LOG" 2>&1
        sleep 5
    done
) &
