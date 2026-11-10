#!/system/bin/sh
# Runs in post-fs-data: make sure the runtime dir exists before anything logs to it.
MODDIR=${0%/*}

mkdir -p /data/adb/codm
chmod 700 /data/adb/codm

# a pid from the previous boot may now belong to an unrelated process
rm -f /data/adb/codm/injector.pid

# config lives outside the module so a module update does not wipe the mapping
if [ ! -f /data/adb/codm/codm.cfg ] && [ -f "$MODDIR/codm.cfg" ]; then
    cp "$MODDIR/codm.cfg" /data/adb/codm/codm.cfg
    chmod 600 /data/adb/codm/codm.cfg
fi
