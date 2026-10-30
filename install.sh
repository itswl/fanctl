#!/bin/sh
# fanctl installer.
#
#   curl -fsSL <latest release>/install.sh | sh
#   curl -fsSL <latest release>/install.sh | sh -s -- --preset quiet
#   sh install.sh [--preset quiet|balanced|cool|lap]
#
# The copy attached to each release carries the whole package (prebuilt binary, presets,
# LaunchDaemon), so nothing is downloaded or compiled. It asks for your password, installs and
# starts the daemon. A new install gets the balanced preset; an existing
# /usr/local/etc/fancontrol.conf is kept unless --preset is given (the old file is then saved
# as fancontrol.conf.bak).
#
# Everything runs from the last line, so a download that breaks off half-way does nothing.
set -eu

PKG=fanctl-macos-arm64
LABEL=local.fancontrol
PLIST=/Library/LaunchDaemons/$LABEL.plist
CONF=/usr/local/etc/fancontrol.conf
SHARE=/usr/local/share/fanctl

die() { echo "fanctl install: $*" >&2; exit 1; }

# payload: begin  (package.sh puts the release package here; a plain checkout has none)
PAYLOAD_SHA256=
payload() { return 1; }
# payload: end

# As the invoking user: unpack the embedded package, then run its installer as root.
from_payload() {
    [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || die "fanctl needs an Apple Silicon Mac"
    [ "$(sw_vers -productVersion | cut -d. -f1)" -ge 14 ] || die "fanctl needs macOS 14 or later"
    work=$(mktemp -d /tmp/fanctl-install.XXXXXX)
    trap 'rm -rf "$work"' EXIT
    payload > "$work/$PKG.tar.gz" || die "the installer is damaged, download it again"
    [ "$(shasum -a 256 "$work/$PKG.tar.gz" | cut -d' ' -f1)" = "$PAYLOAD_SHA256" ] ||
        die "checksum mismatch, download the installer again"
    tar -xzf "$work/$PKG.tar.gz" -C "$work"
    echo "==> $("$work/$PKG/fanctl" version): installing (sudo asks for your password)"
    sudo /bin/sh "$work/$PKG/install.sh" ${PRESET:+--preset "$PRESET"}
}

# As root, from an unpacked package: install the files and (re)start the LaunchDaemon.
from_package() {
    here=$1
    xattr -dr com.apple.quarantine "$here" 2>/dev/null || true    # browser downloads carry it
    install -d -m 755 /usr/local/bin /usr/local/etc "$SHARE/presets"
    install -m 755 "$here/fanctl" /usr/local/bin/fanctl
    rm -f "$SHARE"/presets/*.conf
    install -m 644 "$here"/presets/*.conf "$SHARE/presets/"
    install -m 755 "$here/uninstall.sh" "$SHARE/uninstall.sh"

    if [ -n "$PRESET" ]; then
        [ -f "$SHARE/presets/$PRESET.conf" ] ||
            die "no preset '$PRESET' (available: $(ls "$SHARE/presets" | sed 's/\.conf$//' | tr '\n' ' '))"
        if [ -f "$CONF" ]; then cp -p "$CONF" "$CONF.bak"; echo "    previous config saved as $CONF.bak"; fi
        install -m 644 "$SHARE/presets/$PRESET.conf" "$CONF"
        echo "    config: $PRESET preset"
    elif [ -f "$CONF" ]; then
        echo "    config: keeping $CONF"
    else
        install -m 644 "$SHARE/presets/balanced.conf" "$CONF"
        echo "    config: balanced preset"
    fi
    /usr/local/bin/fanctl check >/dev/null || die "$CONF is invalid; fix it or rerun with --preset balanced"

    echo "==> starting the LaunchDaemon"
    install -m 644 -o root -g wheel "$here/$LABEL.plist" "$PLIST"
    launchctl bootout "system/$LABEL" 2>/dev/null || true         # an older version, if running
    i=0
    while launchctl print "system/$LABEL" >/dev/null 2>&1; do      # bootout returns early
        i=$((i + 1)); [ $i -le 40 ] || die "the old daemon did not stop"
        sleep 0.5
    done
    /usr/local/bin/fanctl start >/dev/null || die "the daemon did not start; see /var/log/fancontrol.log"

    echo
    /usr/local/bin/fanctl status
    echo
    echo "presets    fanctl preset          switch: sudo fanctl preset quiet"
    echo "now        sudo fanctl stop       sudo fanctl start"
    echo "at boot    sudo fanctl autostart off | on"
    echo "log        /var/log/fancontrol.log"
    echo "uninstall  sudo $SHARE/uninstall.sh"
}

main() {
    PRESET=
    while [ $# -gt 0 ]; do
        case $1 in
            --preset) [ $# -ge 2 ] || die "--preset needs a name"; PRESET=$2; shift 2 ;;
            -h|--help) echo "usage: sh install.sh [--preset quiet|balanced|cool|lap]"; exit 0 ;;
            *) die "unknown option: $1 (usage: sh install.sh [--preset NAME])" ;;
        esac
    done
    case $PRESET in
        ""|quiet|balanced|cool|lap) ;;
        *) die "unknown preset '$PRESET' (quiet, balanced, cool or lap)" ;;
    esac

    # Work from / : the caller's directory may be unreadable for root (privacy-protected
    # folders) or gone, and nothing here needs it.
    if [ -n "$PAYLOAD_SHA256" ]; then
        cd / 2>/dev/null
        from_payload
        return
    fi
    here=$(cd "$(dirname "$0")" && pwd)
    cd / 2>/dev/null
    if [ -x "$here/fanctl" ] && [ -f "$here/$LABEL.plist" ] && [ ! -f "$here/fanctl.c" ]; then
        [ "$(id -u)" -eq 0 ] || exec sudo /bin/sh "$here/install.sh" ${PRESET:+--preset "$PRESET"}
        echo "==> installing $("$here/fanctl" version)"
        from_package "$here"
        return
    fi
    die "this copy carries no package; use the one-line install from the README"
}

main "$@"
