#!/system/bin/sh
# Magisk uninstall hook.

PIDFILE=/data/adb/codm/injector.pid
if [ -f "$PIDFILE" ]; then
    kill "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
fi
pkill -f codm-inject 2>/dev/null

rm -rf /data/adb/codm
