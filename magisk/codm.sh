#!/system/bin/sh
# Control + launch script.
#
#   sh codm.sh                  full sequence: stop game, restart daemon,
#                               capture logcat, launch, wait for the overlay
#   sh codm.sh go [package]     same, with an explicit package
#   sh codm.sh status|start|stop|restart|log|pull|cfg
#
# The short version: a running game keeps the old payload mapped, and the
# daemon will not inject into a process that already has libcodm. Nothing
# short of a force-stop can re-arm it, so `go` is the only action that puts
# the overlay on screen.

MODDIR=${0%/*}
BIN="$MODDIR/bin/codm-inject"
STATE=/data/adb/codm
PIDFILE="$STATE/injector.pid"
LOG="$STATE/injector.log"
CFG="$STATE/codm.cfg"
CAP=/sdcard/codm.log

PKGS="com.garena.game.codm com.activision.callofduty.shooter com.tencent.tmgp.cod com.vng.codm"

alive() {
    [ -f "$PIDFILE" ] || return 1
    p=$(cat "$PIDFILE" 2>/dev/null)
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null
}

start_daemon() {
    if alive; then
        echo "daemon: already running (pid $(cat "$PIDFILE"))"
        return 0
    fi
    mkdir -p "$STATE"
    chmod 700 "$STATE"
    : > "$LOG"
    nohup "$BIN" >> "$LOG" 2>&1 &
    sleep 1
    if alive; then
        echo "daemon: started (pid $(cat "$PIDFILE"))"
        return 0
    fi
    echo "daemon: failed to start"
    tail -n 20 "$LOG" 2>/dev/null
    return 1
}

stop_daemon() {
    # pidfile only: once the comm/argv disguise lands, pkill -f finds nothing
    if [ -f "$PIDFILE" ]; then
        p=$(cat "$PIDFILE" 2>/dev/null)
        [ -n "$p" ] && kill "$p" 2>/dev/null
    fi
    rm -f "$PIDFILE"
}

pick_pkg() {
    if [ -n "$1" ]; then
        echo "$1"
        return 0
    fi
    for p in $PKGS; do
        if [ -n "$(pidof "$p" 2>/dev/null)" ]; then
            echo "$p"
            return 0
        fi
    done
    inst=$(pm list packages 2>/dev/null | sed 's/^package://')
    for p in $PKGS; do
        if echo "$inst" | grep -qx "$p"; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

# wait_for <seconds> <file> <extended-regex> <label>
wait_for() {
    n=0
    while [ "$n" -lt "$1" ]; do
        if grep -qE "$3" "$2" 2>/dev/null; then
            echo "  ok    $4"
            return 0
        fi
        sleep 1
        n=$((n + 1))
    done
    echo "  FAIL  $4 (${1}s)"
    return 1
}

go() {
    pkg=$(pick_pkg "$1")
    if [ -z "$pkg" ]; then
        echo "no CODM package found (tried: $PKGS)"
        echo "pass one: sh $0 go <package>"
        return 1
    fi
    echo "== codm go — $pkg"

    # 1. the old process holds the old payload; the daemon never re-injects it
    echo "-- stop game"
    am force-stop "$pkg" >/dev/null 2>&1
    n=0
    while [ -n "$(pidof "$pkg" 2>/dev/null)" ] && [ "$n" -lt 20 ]; do
        sleep 1
        n=$((n + 1))
    done
    if [ -n "$(pidof "$pkg" 2>/dev/null)" ]; then
        echo "   still alive after 20s, SIGKILL"
        for p in $(pidof "$pkg" 2>/dev/null); do
            kill -9 "$p" 2>/dev/null
        done
        sleep 2
    fi

    # 2. fresh daemon, fresh log, so nothing below can match a previous run
    echo "-- restart daemon"
    stop_daemon
    sleep 1
    start_daemon || return 1

    # 3. capture before launch: ColorOS rotates the buffer in a few minutes
    quiet=$(grep -E '^stealth\.quiet' "$CFG" 2>/dev/null | tail -n 1 | tr -dc '0-9')
    logcat -c 2>/dev/null
    : > "$CAP" 2>/dev/null || { CAP=/data/local/tmp/codm.log; : > "$CAP"; }
    logcat -s codm:V > "$CAP" 2>&1 &
    cap=$!

    # 4. launch
    echo "-- launch"
    act=$(cmd package resolve-activity --brief "$pkg" 2>/dev/null | tail -n 1)
    case "$act" in
        */*) am start -n "$act" >/dev/null 2>&1 ;;
    esac
    sleep 2
    if [ -z "$(pidof "$pkg" 2>/dev/null)" ]; then
        monkey -p "$pkg" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    fi

    n=0
    while [ -z "$(pidof "$pkg" 2>/dev/null)" ] && [ "$n" -lt 60 ]; do
        sleep 1
        n=$((n + 1))
    done
    if [ -z "$(pidof "$pkg" 2>/dev/null)" ]; then
        echo "  FAIL  process never came up"
        kill "$cap" 2>/dev/null
        return 1
    fi
    echo "  ok    pid=$(pidof "$pkg")"

    # 5. wait the chain: inject -> il2cpp -> hooks -> first frame
    ok=1
    wait_for 120 "$LOG" "injected .* pid=" "injected" || ok=0
    if [ "$quiet" = "1" ]; then
        echo "  --    stealth.quiet=1, payload logging is off"
        echo "        overlay state can't be read; tap 3 fingers in-game"
    else
        wait_for 240 "$CAP" "payload armed" "payload armed" || ok=0
        wait_for 90 "$CAP" "imgui up" "imgui up (gles3)" || ok=0
    fi
    kill "$cap" 2>/dev/null

    echo
    if [ "$ok" = "1" ]; then
        echo "overlay is up — 3-finger tap toggles the menu"
        echo "log: $CAP"
        return 0
    fi

    echo "did not reach the overlay."
    if grep -q "hook failed" "$CAP" 2>/dev/null; then
        echo "hint: render hook refused — the game is likely on Vulkan; this build only hooks GLES"
    elif grep -q "libil2cpp" "$CAP" 2>/dev/null; then
        echo "hint: Unity was late — get past the download/login shell into the lobby, then run it again"
    fi
    echo "--- payload log ($CAP) ---"
    tail -n 25 "$CAP" 2>/dev/null
    echo "--- injector log ---"
    tail -n 12 "$LOG" 2>/dev/null
    return 1
}

case "$1" in
go)
    go "$2"
    exit $?
    ;;
status)
    if alive; then
        echo "daemon: running (pid $(cat "$PIDFILE"))"
    else
        echo "daemon: stopped"
    fi
    for p in $PKGS; do
        pids=$(pidof "$p" 2>/dev/null)
        [ -z "$pids" ] && continue
        pid=${pids%% *}
        echo "game:   $p pid=$pid libcodm=$(grep -c libcodm /proc/$pid/maps 2>/dev/null) libil2cpp=$(grep -c libil2cpp /proc/$pid/maps 2>/dev/null)"
    done
    echo "--- last log lines ---"
    tail -n 15 "$LOG" 2>/dev/null
    ;;
start)
    start_daemon
    ;;
stop)
    stop_daemon
    echo "stopped"
    ;;
restart)
    stop_daemon
    sleep 1
    start_daemon
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
    ${EDITOR:-vi} "$CFG"
    ;;
"")
    go ""
    exit $?
    ;;
*)
    echo "usage: $0 [go [package]]|status|start|stop|restart|log|pull|cfg"
    ;;
esac
