#!/system/bin/sh
# late_start service: wait for boot, then run the injector daemon under a watchdog.
MODDIR=${0%/*}

LOG=/data/adb/codm/injector.log
PIDFILE=/data/adb/codm/injector.pid

mkdir -p /data/adb/codm
chmod 700 /data/adb/codm

while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 3
done

# one daemon per boot
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
    exit 0
fi

(
    while true; do
        "$MODDIR/bin/codm-inject" >> "$LOG" 2>&1 &
        child=$!
        echo "$child" > "$PIDFILE"
        wait "$child"
        sleep 5
    done
) &
