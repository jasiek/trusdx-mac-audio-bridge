#!/bin/sh
# Builds and installs truSDX.driver (sudo, only when it changed) and the
# truSDX Bridge menu bar app, then launches the app.
set -eu

cd "$(dirname "$0")/.."
BUILD=build
HAL=/Library/Audio/Plug-Ins/HAL
APP_DIR="$HOME/Applications"
OLD_AGENT=com.github.trusdx-mac-audio-bridge

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j
"$BUILD/trusdx_tests"

if cmp -s "$BUILD/truSDX.driver/Contents/MacOS/truSDX" "$HAL/truSDX.driver/Contents/MacOS/truSDX"; then
    echo "Driver unchanged; skipping."
else
    echo "Installing driver into $HAL (sudo)..."
    sudo rm -rf "$HAL/truSDX.driver"
    sudo cp -R "$BUILD/truSDX.driver" "$HAL/"
    sudo killall coreaudiod # launchd restarts it and it loads the new driver
fi

# Replace any running bridge (the app, a terminal instance, or the old login agent).
launchctl bootout "gui/$(id -u)/$OLD_AGENT" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/$OLD_AGENT.plist"
pkill -INT -x TruSDXBridge 2>/dev/null && sleep 1 || true

echo "Installing app into $APP_DIR..."
mkdir -p "$APP_DIR"
rm -rf "$APP_DIR/TruSDXBridge.app"
cp -R "$BUILD/TruSDXBridge.app" "$APP_DIR/"
open "$APP_DIR/TruSDXBridge.app"
echo "truSDX Bridge is in the menu bar. Use its menu for Start/Stop and Open at Login."
