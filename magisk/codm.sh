#!/system/bin/sh
# Control script. Usage:
#   sh /data/adb/modules/codm_overlay/codm.sh status|start|stop|restart|log|pull|cfg

MODDIR=${0%/*}
BIN="$MODDIR/bin/codm-inject"
PIDFILE=/data/adb/codm/injector.pid
LOG=/data/adb/codm/injector.log

PKGS="com.activision.callofduty.shooter com.garena.game.codm com.tencent.tmgp.cod com.vng.codm"

running() {
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
}

case "$1" in
status)
    if running; then
        echo "daemon: running (pid $(cat "$PIDFILE"))"
    else
        echo "daemon: stopped"
    fi
    for p in $PKGS; do
        pid=$(pidof "$p" 2>/dev/null)
        [ -n "$pid" ] && echo "game:   $p pid=$pid"
    done
    echo "--- last log lines ---"
    tail -n 15 "$LOG" 2>/dev/null
    ;;
start)
    if running; then
        echo "already running"
        exit 0
    fi
    mkdir -p /data/adb/codm
    nohup "$BIN" >> "$LOG" 2>&1 &
    echo $! > "$PIDFILE"
    echo "started pid $(cat "$PIDFILE")"
    ;;
stop)
    [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null
    pkill -f codm-inject 2>/dev/null
    rm -f "$PIDFILE"
    echo "stopped"
    ;;
restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
log)
    tail -n 80 -f "$LOG"
    ;;
pull)
    for p in $PKGS; do
        f="/data/data/$p/files/codm_dump.txt"
        if [ -f "$f" ]; then
            cp "$f" /sdcard/codm_dump.txt
            echo "copied $f -> /sdcard/codm_dump.txt"
            exit 0
        fi
    done
    echo "no dump yet — open the menu in-game and press 'Dump il2cpp'"
    ;;
cfg)
    ${EDITOR:-vi} /data/adb/codm/codm.cfg
    ;;
*)
    echo "usage: $0 status|start|stop|restart|log|pull|cfg"
    ;;
esac
