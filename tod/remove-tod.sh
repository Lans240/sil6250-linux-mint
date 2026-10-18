#!/usr/bin/env bash
set -euo pipefail

TOD_DIR="$(pkg-config --variable=tod_driversdir libfprint-2-tod-1)"
PLUGIN="$TOD_DIR/libfprint-2-tod-sil6250.so"
DROPIN=/etc/systemd/system/fprintd.service.d/60-sil6250.conf

if [[ $EUID -ne 0 ]]; then
  echo "Запусти через sudo: sudo bash $0" >&2
  exit 1
fi
rm -f -- "$PLUGIN" "$DROPIN"
rmdir --ignore-fail-on-non-empty /etc/systemd/system/fprintd.service.d 2>/dev/null || true
systemctl daemon-reload
systemctl try-restart fprintd.service || true
echo "Удалены только файл TOD-драйвера SIL6250 и его systemd drop-in."
