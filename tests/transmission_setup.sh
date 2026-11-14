#!/bin/sh
# transmission_setup.sh - Transmission settings before the first start: no port mapping on the router
# (UPnP/NAT-PMP off), no update check on the network (Sparkle). Existing settings are backed up first.
P=~/Library/Preferences/org.m0k.transmission.plist
if [ -f $P ]; then
    cp -p $P $P.bak && echo "backed up: $P.bak"
fi
defaults write org.m0k.transmission NatTraversal -bool NO
defaults write org.m0k.transmission SUEnableAutomaticChecks -bool NO
defaults write org.m0k.transmission SUCheckAtStartup -bool NO
defaults read org.m0k.transmission
