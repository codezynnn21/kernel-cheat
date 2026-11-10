#!/system/bin/sh
# Magisk installer hook. $MODPATH is set by the Magisk installer.

ui_print " "
ui_print "  CODM overlay — il2cpp ESP"
ui_print "  arm64 / Android 12+ / root"
ui_print " "

set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
set_perm "$MODPATH/codm.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755

# runtime state dir + seed config (never clobber an edited one)
mkdir -p /data/adb/codm
chmod 700 /data/adb/codm
if [ -f /data/adb/codm/codm.cfg ]; then
    ui_print "- keeping existing /data/adb/codm/codm.cfg"
else
    cp "$MODPATH/codm.cfg" /data/adb/codm/codm.cfg
    chmod 600 /data/adb/codm/codm.cfg
    ui_print "- seeded /data/adb/codm/codm.cfg"
fi

ui_print "- binaries:"
ui_print "    bin/codm-inject  (root daemon)"
ui_print "    assets/libcodm.so (payload)"
ui_print "- config: /data/adb/codm/codm.cfg"
ui_print "- reboot, then launch CODM"
