#!/usr/bin/env bash
# Put the stock firmware back: writes this chip's backup (backup_stock.sh) at 0x0.
#   ./restore_stock.sh <serial-port> <backup-dir>
exec python "$(cd "$(dirname "$0")" && pwd)/fanflash.py" restore "$@"
