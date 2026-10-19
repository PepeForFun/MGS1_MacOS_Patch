#!/bin/sh
# Metal Gear Solid for Mac: builds the two apps from your own GOG copy of the game.
# Put this folder into the game folder (next to mgsi.exe) and double-click this file,
# or run: sh Build.command [game folder]
cd "$(dirname "$0")" || exit 1
fail() { echo; echo "Stopped: $1"; echo; exit 1; }
step() { echo; echo "== $1"; }
logtail() { echo; tail -15 "$1"; fail "$2 (full log: $PWD/$1)"; }

GAME=$1
if [ -z "$GAME" ] && [ -f ../mgsi.exe ]; then GAME=$(cd .. && pwd); fi
if [ -z "$GAME" ]; then
    GAME=$(osascript -e 'POSIX path of (choose folder with prompt "Choose the Metal Gear Solid game folder (the one with mgsi.exe in it)")' 2>/dev/null) ||
        fail "no game folder chosen"
fi
GAME=${GAME%/}
[ -f "$GAME/mgsi.exe" ] || fail "mgsi.exe is not in $GAME. Put this folder into the game folder and try again."
echo "Game folder: $GAME"

[ "$(uname -m)" = arm64 ] || echo "Note: this Mac is not Apple Silicon; the build is untested there."
if ! xcrun --find clang > /dev/null 2>&1; then
    xcode-select --install > /dev/null 2>&1
    fail "Apple's Command Line Tools are needed. Install them from the window that just opened (or run \"xcode-select --install\" in Terminal), then start Build.command again."
fi
PY=/usr/bin/python3
JOBS=$(sysctl -n hw.ncpu)
T0=$(date +%s)
mkdir -p build

# the GOG release these were made and tested with
check() {
    [ "$(shasum -a 256 "$GAME/$1" | cut -d' ' -f1)" = "$2" ] ||
        echo "Note: $1 differs from the GOG release this was tested with; trying anyway."
}
check mgsi.exe 795f92c157d820fca62740f2144b45dd14a77d480585b60e3fec53475064f23e

step "Translating Metal Gear Solid (mgsi.exe)"
"$PY" -I tools/recomp.py "$GAME/mgsi.exe" build/gen > build/recomp-main.log 2>&1 || logtail build/recomp-main.log "translating mgsi.exe failed"
step "Compiling it (a few minutes)"
make -j"$JOBS" > build/make-main.log 2>&1 || logtail build/make-main.log "compiling failed"

VR=
if [ -f "$GAME/mgsvr.exe" ]; then
    check mgsvr.exe 6037d40def319ef19470a5bd52fc29c4bd996ebb3bcf12aed44d1bd7c7c9a3e3
    step "Translating VR Missions (mgsvr.exe)"
    "$PY" -I tools/recomp.py "$GAME/mgsvr.exe" build/gen-vr > build/recomp-vr.log 2>&1 || logtail build/recomp-vr.log "translating mgsvr.exe failed"
    step "Compiling it"
    make -j"$JOBS" GEN=build/gen-vr BIN=build/mgsvr > build/make-vr.log 2>&1 || logtail build/make-vr.log "compiling VR Missions failed"
    VR=1
fi

step "Making the apps"
sh tools/make_app.sh "$GAME" "$GAME" > build/apps.log 2>&1 || logtail build/apps.log "making Metal Gear Solid.app failed"
if [ -n "$VR" ]; then
    NAME="Metal Gear Solid VR Missions" BIN=build/mgsvr ID=io.github.mgsrecomp.mac.vr \
        sh tools/make_app.sh "$GAME" "$GAME" >> build/apps.log 2>&1 || logtail build/apps.log "making the VR Missions app failed"
fi
cp README-player.txt "$GAME/Metal Gear Solid for Mac - README.txt"

# a personal copy of the patch can carry ready-made HD textures: cloned into the game folder
# (no extra disk space on the same drive), files already there are kept
HD=
if [ -d "HD Textures" ]; then
    step "Installing the HD textures"
    mkdir -p "$GAME/HD Textures"
    cp -c -n -R "HD Textures/." "$GAME/HD Textures/" 2> /dev/null ||
        cp -n -R "HD Textures/." "$GAME/HD Textures/" || fail "copying the HD textures failed"
    HD=$(ls "$GAME/HD Textures" | grep -c '\.png$')
fi

S=$(( $(date +%s) - T0 ))
echo
echo "Done in $((S / 60)) min $((S % 60)) s. In the game folder:"
echo "  Metal Gear Solid.app"
[ -n "$VR" ] && echo "  Metal Gear Solid VR Missions.app"
echo "  Metal Gear Solid for Mac - README.txt"
[ -n "$HD" ] && echo "  HD Textures ($HD files)"
echo "The apps must stay in the game folder. Keep them for yourself: they contain code translated"
echo "from your copy of the game."
