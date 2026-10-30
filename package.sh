#!/bin/sh
# Builds the release files.
#
#   ./package.sh [OUTDIR]      (default: ./dist)
#
#   OUTDIR/install.sh                   one-file installer: install.sh with the package inside
#   OUTDIR/fanctl-macos-arm64.tar.gz    the package on its own
#   OUTDIR/SHA256SUMS
#
# Build releases from a clean checkout so the files match the tagged commit.
set -eu

SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$SRC/dist}
NAME=fanctl-macos-arm64
STAGE=$OUT/$NAME
VERSION=$(sed -n 's/^#define FANCTL_VERSION "\(.*\)"/\1/p' "$SRC/fanctl.c")

rm -rf "$STAGE" "$OUT/$NAME.tar.gz" "$OUT/install.sh" "$OUT/SHA256SUMS"
mkdir -p "$STAGE/presets"
clang -O2 -Wall -Wextra -arch arm64 -mmacosx-version-min=14.0 -o "$STAGE/fanctl" "$SRC/fanctl.c" \
    -framework IOKit -framework CoreFoundation
cp "$SRC/install.sh" "$SRC/uninstall.sh" "$SRC/local.fancontrol.plist" \
    "$SRC/README.md" "$SRC/README.zh-CN.md" "$STAGE/"
cp "$SRC"/presets/*.conf "$STAGE/presets/"
chmod 755 "$STAGE/fanctl" "$STAGE/install.sh" "$STAGE/uninstall.sh"
(cd "$OUT" && COPYFILE_DISABLE=1 tar -czf "$NAME.tar.gz" "$NAME")

# The one-file installer: install.sh with its payload block replaced by the package.
SUM=$(shasum -a 256 "$OUT/$NAME.tar.gz" | cut -d' ' -f1)
{
    sed -n '1,/^# payload: begin/p' "$SRC/install.sh"
    echo "PAYLOAD_SHA256=$SUM"
    echo "payload() {"
    echo "base64 -D <<'FANCTL_PAYLOAD'"
    base64 -b 76 -i "$OUT/$NAME.tar.gz"
    echo "FANCTL_PAYLOAD"
    echo "}"
    sed -n '/^# payload: end/,$p' "$SRC/install.sh"
} > "$OUT/install.sh"
chmod 755 "$OUT/install.sh"

(cd "$OUT" && shasum -a 256 install.sh "$NAME.tar.gz" "$NAME/fanctl" > SHA256SUMS)
echo "fanctl $VERSION -> $OUT/install.sh ($(wc -c < "$OUT/install.sh" | tr -d ' ') bytes)"
cat "$OUT/SHA256SUMS"
