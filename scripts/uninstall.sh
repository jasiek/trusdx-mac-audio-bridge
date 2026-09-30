#!/bin/sh
# Removes the driver, the app and the login agent.
set -u

AGENT_LABEL=com.github.trusdx-mac-audio-bridge
launchctl bootout "gui/$(id -u)/$AGENT_LABEL" 2>/dev/null
rm -f "$HOME/Library/LaunchAgents/$AGENT_LABEL.plist"
rm -rf "$HOME/Applications/TruSDXBridge.app"
sudo rm -rf /Library/Audio/Plug-Ins/HAL/truSDX.driver
sudo killall coreaudiod
echo "Uninstalled."
