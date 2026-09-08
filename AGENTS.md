# PlutoBrowser Development Rules

## Deploy to Physical Device

0. **ALWAYS check `/Volumes/PLAYDATE` is mounted first** — if not mounted, the device may already be in data-disk mode or not connected. Do NOT assume — verify with `ls /Volumes/PLAYDATE/`.
1. Put device in data-disk mode via serial: `/Users/bwandrych/Developer/PlaydateSDK/bin/pdutil /dev/cu.usbmodemPDU1_Y0738581 datadisk`
2. Wait 8+ seconds for `/Volumes/PLAYDATE` to mount
3. Delete old PDX: `rm -rf /Volumes/PLAYDATE/Games/PlutoBrowser.pdx`
4. Wait 10 seconds
5. Copy ENTIRE PDX: `cp -R /Users/bwandrych/Desktop/PlutoBrowser/PlutoBrowser.pdx /Volumes/PLAYDATE/Games/`
6. **VERIFY `pdex.bin` MD5 MATCHES before ejecting — NEVER eject until this check passes**
7. Eject: `diskutil eject /Volumes/PLAYDATE`
8. Wait 60 seconds
9. Launch: `/Users/bwandrych/Developer/PlaydateSDK/bin/pdutil /dev/cu.usbmodemPDU1_Y0738581 run Games/PlutoBrowser.pdx`

## Verify Command (md5 of the device binary — the only file that matters)
```bash
SRC_MD5=$(md5 -q /Users/bwandrych/Desktop/PlutoBrowser/PlutoBrowser.pdx/pdex.bin)
DST_MD5=$(md5 -q /Volumes/PLAYDATE/Games/PlutoBrowser.pdx/pdex.bin)
echo "SRC: $SRC_MD5  DST: $DST_MD5"
```
The MD5 hashes MUST match. If they don't, re-copy and verify again. Do NOT eject until they match.

**NOTE:** `du -sb` does NOT work on macOS. File counts will also differ (pdc copies .c/.h source files into the PDX). Only `pdex.bin` MD5 matters.

## Simulator Log Path
`/Users/bwandrych/Developer/PlaydateSDK/Disk/Data/com.bryanwandrych.plutobrowser/pluto.log`

## Log Collection (Device)
- **ALWAYS check `/Volumes/PLAYDATE` is mounted before reading logs**
- Read: `/Volumes/PLAYDATE/Data/com.bryanwandrych.plutobrowser/pluto.log`
- Also check: `/Volumes/PLAYDATE/crashlog.txt` and `/Volumes/PLAYDATE/errorlog.txt`
- Clear all 3 log files before EVERY re-deploy

## Key Rules
- NEVER conclude device is disconnected from WiFi
- 60-second wait after ejecting data disk before launching app
- **TERMINATE SIMULATOR IMMEDIATELY AFTER EVERY TEST.** Never leave simulator running. Never have more than one simulator process active. Kill it right after checking logs — do not wait. Use `kill -9 <PID>` (NOT `pkill` — it doesn't work on macOS for Playdate Simulator). Verify with `ps aux | grep -i "playdate simulator" | grep -v grep`.
- Test in simulator first, then deploy to device
- Recompile PDX every code edit before copying to device
- Never run git commands — user handles all git operations
- Log everything — do NOT remove log statements unless user says to
