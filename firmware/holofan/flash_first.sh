#!/usr/bin/env bash
# One-time wired flash of the HoloFan layout: our rollback bootloader (0x0),
# partition table (0x8000), otadata (0x1F0000) and the app in ota_0 (0x10000).
# NVS is never written. Refuses without a verified backup of this chip
# (backup_stock.sh). After this, every update is an OTA (docs/MAINTENANCE_API.md).
#   ./flash_first.sh <serial-port> <backup-dir>
exec python "$(cd "$(dirname "$0")" && pwd)/fanflash.py" first "$@"
