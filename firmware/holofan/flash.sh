#!/usr/bin/env bash
# Recovery only: write build/holofan.bin into ota_0 (0x10000). After an OTA the
# fan may be booting ota_1, so prefer an OTA in maintenance mode. Refuses
# without a verified backup of this chip (backup_stock.sh).
#   ./flash.sh <serial-port> <backup-dir>
exec python "$(cd "$(dirname "$0")" && pwd)/fanflash.py" app "$@"
