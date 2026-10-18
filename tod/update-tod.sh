#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_SRC="$ROOT/tod/driver/libfprint-2-tod-sil6250.so"
TOD_DIR="$(pkg-config --variable=tod_driversdir libfprint-2-tod-1)"
PLUGIN_DST="$TOD_DIR/libfprint-2-tod-sil6250.so"
DROPIN=/etc/systemd/system/fprintd.service.d/60-sil6250.conf
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$PLUGIN_DST.backup-$STAMP"
TEMP="$PLUGIN_DST.new-$STAMP"

if [[ $EUID -ne 0 ]]; then
  echo "Запусти через sudo: sudo bash $0" >&2
  exit 1
fi
if [[ ! -f "$PLUGIN_SRC" || ! -f "$PLUGIN_DST" || ! -f "$DROPIN" ]]; then
  echo "Не найдены собранный драйвер или установленная конфигурация; ничего не меняю." >&2
  exit 1
fi

cp -a -- "$PLUGIN_DST" "$BACKUP"
install -m 0644 "$PLUGIN_SRC" "$TEMP"
mv -f -- "$TEMP" "$PLUGIN_DST"
systemctl daemon-reload
systemctl reset-failed fprintd.service || true
if ! systemctl start fprintd.service; then
  mv -f -- "$BACKUP" "$PLUGIN_DST"
  systemctl daemon-reload
  systemctl reset-failed fprintd.service || true
  systemctl start fprintd.service || true
  echo "Запуск не удался; предыдущий драйвер восстановлен." >&2
  exit 1
fi
echo "Драйвер обновлён. Резервная копия: $BACKUP"
echo "Проверь снова: fprintd-enroll -f right-index-finger \"\$USER\""
