#!/bin/sh
# Moto G6 Plus, iOS 16 root: move mediaserverd's Bluetooth audio HAL plug-in out of the way.
# With com.apple.bluetoothd disabled (no Bluetooth driver; tools/g6plus/disable_jobs.py) the plug-in retries its
# XPC connection in a tight loop: mediaserverd ~48 % and launchd ~73 % of a core for ever (spindump 2026-11-18:
# queue "com.apple.BTAudioHALPlugin" blocked on launchd's event queue). Run against the booted phone; undo by
# moving the bundle back from HAL.off. Changes only the root image served by the Mac.
set -e
HOST=${1:-root@192.168.7.2}
ssh -o BatchMode=yes "$HOST" 'H=/System/Library/Audio/Plug-Ins; if [ -d $H/HAL/BTAudioHALPlugin.driver ]; then mkdir -p $H/HAL.off && mv $H/HAL/BTAudioHALPlugin.driver $H/HAL.off/ && sync && echo "moved; restarting mediaserverd" && kill -9 $(ps -Ao pid,comm | grep /usr/sbin/mediaserverd | grep -v grep | cut -c1-6); else echo "already moved"; fi'
