#!/bin/sh
# Builds the release files into dist/ and the web installer into site/:
#   tapedeck-<ver>.bin         full image (bootloader, partition table, app):
#                              flashed at offset 0 by the web installer or
#                              esptool; erases settings and the job history
#   tapedeck-<ver>-update.bin  the app alone, for Setup > Firmware on the
#                              device's web page (or curl -F fw=@... /update);
#                              keeps settings and history
# The version comes from TAPEDECK_VERSION in platformio.ini.
# Usage: tools/release.sh            (then tools/publish_site.sh to push Pages)
set -e
cd "$(dirname "$0")/.."
ver=$(sed -n 's/.*TAPEDECK_VERSION=\\"\([^\\]*\)\\".*/\1/p' platformio.ini)
[ -n "$ver" ] || { echo "no TAPEDECK_VERSION in platformio.ini" >&2; exit 1; }
~/.platformio/penv/bin/pio run -e app
rm -rf dist site && mkdir -p dist site
cp .pio/build/app/firmware.factory.bin "dist/tapedeck-$ver.bin"
cp .pio/build/app/firmware.bin "dist/tapedeck-$ver-update.bin"
cp "dist/tapedeck-$ver.bin" "site/tapedeck-$ver.bin"
sed -e "s/@VER@/$ver/g" web-installer/index.html > site/index.html
sed -e "s/@VER@/$ver/g" web-installer/manifest.json > site/manifest.json
cp docs/shots/history.png site/
touch site/.nojekyll
ls -la dist site
