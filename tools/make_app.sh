#!/bin/sh
# Build "Metal Gear Solid.app" from build/mgs. The app finds the game in the folder that holds it
# (or a folder above), so it goes inside the GOG game folder next to mgsi.exe.
# usage: tools/make_app.sh <game folder with goggame-*.ico> [output dir]
set -e
cd "$(dirname "$0")/.."
GAME=${1:?game folder}
OUT=${2:-build}
NAME=${NAME:-Metal Gear Solid}
BIN=${BIN:-build/mgs}
ID=${ID:-io.github.mgsrecomp.mac}
APP="$OUT/$NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/mgs"

ICO=$(ls "$GAME"/app/goggame-*.ico "$GAME"/goggame-*.ico 2>/dev/null | head -1 || true)
if [ -n "$ICO" ]; then
    SET=$(mktemp -d)/AppIcon.iconset
    mkdir -p "$SET"
    sips -s format png "$ICO" --out "$SET/base.png" >/dev/null
    for s in 16 32 128 256 512; do
        sips -z $s $s "$SET/base.png" --out "$SET/icon_${s}x${s}.png" >/dev/null
        d=$((s * 2))
        sips -z $d $d "$SET/base.png" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
    done
    rm "$SET/base.png"
    iconutil -c icns "$SET" -o "$APP/Contents/Resources/AppIcon.icns"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>$NAME</string>
    <key>CFBundleDisplayName</key><string>$NAME</string>
    <key>CFBundleIdentifier</key><string>$ID</string>
    <key>CFBundleExecutable</key><string>mgs</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
xattr -cr "$APP"
codesign --force --deep -s - "$APP"
echo "built $APP"
