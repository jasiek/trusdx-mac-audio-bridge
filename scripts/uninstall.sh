#!/bin/sh
# Removes truSDX Bridge: the app, the Core Audio driver and the installer receipt.
# Also shipped inside the app: /Applications/TruSDXBridge.app/Contents/Resources/uninstall.sh
set -u

osascript -e 'quit app id "com.github.trusdx-mac-audio-bridge.bridge"' 2>/dev/null
pkill -x TruSDXBridge 2>/dev/null

# Leftovers from earlier development installs.
launchctl bootout "gui/$(id -u)/com.github.trusdx-mac-audio-bridge" 2>/dev/null
rm -f "$HOME/Library/LaunchAgents/com.github.trusdx-mac-audio-bridge.plist"
rm -rf "$HOME/Applications/TruSDXBridge.app"

sudo rm -rf /Applications/TruSDXBridge.app /Library/Audio/Plug-Ins/HAL/truSDX.driver
sudo pkgutil --forget com.github.trusdx-mac-audio-bridge >/dev/null 2>&1
sudo killall coreaudiod
echo "truSDX Bridge uninstalled."
