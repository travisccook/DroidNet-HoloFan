#!/usr/bin/env bash
# Put the HoloFan into maintenance mode on Wi-Fi profile 1 through a WCB that a
# DroidNet Pi serves, wait until the fan answers on Wi-Fi, then POST an app
# image, retrying (an OTA over a weak link can need 2-4 tries).
#   WCB_HOST=<droidnet-pi> [FAN=<fan-ip-or-name>] tools/bench/ota_fan.sh [image]
# WCB_HOST is the Pi's mDNS name without .local (wcbwatch.py adds it).
# FAN defaults to holofan.local; an IPv4 address avoids slow .local lookups on macOS.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMG=${1:-$REPO/firmware/holofan/build/holofan.bin}
: "${WCB_HOST:?set WCB_HOST to the DroidNet Pi that serves your WCB}"
FAN=${FAN:-holofan.local}
[ -f "$IMG" ] || { echo "no image $IMG: run firmware/holofan/build.sh"; exit 2; }
ENTRY_OK=1
if ! ENTRY_OUT=$(python3 "$HERE/wcbwatch.py" "$WCB_HOST" 3 ';wHoloFan,#HLZX1' 2>&1); then
  ENTRY_OK=0
elif ! printf '%s\n' "$ENTRY_OUT" | grep -q -- '-> True'; then
  ENTRY_OK=0          # wcbwatch.py ran, but the Pi did not report the command as sent
fi
if [ "$ENTRY_OK" -eq 0 ]; then
  echo "WARNING: could not ask the fan for maintenance mode through WCB_HOST=$WCB_HOST"
  echo "  (is the Pi reachable, and is the Python package 'websockets' installed?). wcbwatch.py said:"
  printf '%s\n' "$ENTRY_OUT" | tail -n 5 | sed 's/^/    /'
  echo "  Checking whether the fan is in maintenance mode already ..."
fi
FAN_UP=0
for i in $(seq 1 30); do
  if curl -s -m 3 "http://$FAN/api/info"; then echo; FAN_UP=1; break; fi
  sleep 2
done
if [ "$FAN_UP" -eq 0 ]; then
  if [ "$ENTRY_OK" -eq 0 ]; then
    echo "OTA-FAILED: the maintenance-mode command was not sent, and http://$FAN/api/info does not answer"
  else
    echo "OTA-FAILED: the command was sent, but http://$FAN/api/info did not answer in 60 s or more"
    echo "  (the fan refuses maintenance mode in some states, docs/MAINTENANCE_API.md; or FAN=$FAN is wrong)"
  fi
  exit 1
fi
for t in 1 2 3 4; do
  echo "OTA try $t"
  if curl -s -m 240 -X POST -H 'Content-Type: application/octet-stream' --data-binary @"$IMG" "http://$FAN/api/ota" | tee /dev/stderr | grep -q '"ok":true'; then echo; echo OTA-OK; exit 0; fi
  echo; sleep 3
done
echo OTA-FAILED; exit 1
