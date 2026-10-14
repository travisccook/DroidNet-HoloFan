#!/usr/bin/env bash
# Read the fan's whole 2 MB flash twice and keep it if both reads match (README
# "First flash", step 4). Read-only on the fan. Run after `. $IDF_PATH/export.sh`.
#   ./backup_stock.sh <serial-port> <backup-dir>
exec python "$(cd "$(dirname "$0")" && pwd)/fanflash.py" backup "$@"
