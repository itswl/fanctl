#!/bin/sh
# Stops fanctl, hands the fans back to macOS and removes it.
#
#   sudo ./uninstall.sh [--purge]      --purge also deletes the config and the log
set -eu

[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"
LABEL=local.fancontrol
PURGE=0
case ${1:-} in
    --purge) PURGE=1 ;;
    "") ;;
    *) echo "usage: sudo $0 [--purge]" >&2; exit 2 ;;
esac

echo "==> stopping the daemon"
launchctl bootout "system/$LABEL" 2>/dev/null || true
launchctl enable "system/$LABEL" 2>/dev/null || true    # drop an "autostart off" setting
pkill -TERM -f "/usr/local/bin/fanctl run" 2>/dev/null || true
sleep 1

KEEP_BIN=0
echo "==> handing the fans back to macOS"
if [ -x /usr/local/bin/fanctl ] && ! /usr/local/bin/fanctl auto; then
    KEEP_BIN=1
    echo "    some fans still report manual mode; keeping /usr/local/bin/fanctl so you can retry: sudo fanctl auto"
fi

echo "==> removing files"
rm -f "/Library/LaunchDaemons/$LABEL.plist" /var/run/fanctl.lock
rm -rf /usr/local/share/fanctl
[ "$KEEP_BIN" -eq 1 ] || rm -f /usr/local/bin/fanctl
if [ "$PURGE" -eq 1 ]; then
    rm -f /usr/local/etc/fancontrol.conf /usr/local/etc/fancontrol.conf.bak /var/log/fancontrol.log
else
    echo "    kept /usr/local/etc/fancontrol.conf and /var/log/fancontrol.log (--purge removes them)"
fi
echo "done"
