#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_SRC="$ROOT/tod/driver/libfprint-2-tod-sil6250.so"
TOD_DIR="$(pkg-config --variable=tod_driversdir libfprint-2-tod-1)"
PLUGIN_DST="$TOD_DIR/libfprint-2-tod-sil6250.so"
DROPIN_DIR=/etc/systemd/system/fprintd.service.d
DROPIN="$DROPIN_DIR/60-sil6250.conf"

if [[ $EUID -ne 0 ]]; then
  echo "Запусти этот скрипт через sudo: sudo bash $0" >&2
  exit 1
fi
if ! dpkg-query -W -f='${Status}' fprintd 2>/dev/null | grep -q 'install ok installed'; then
  echo "Штатный пакет fprintd не найден; ничего не меняю." >&2
  exit 1
fi
if ! dpkg-query -W -f='${Status}' fingwit 2>/dev/null | grep -q 'install ok installed'; then
  echo "Пакет Fingwit не найден; ничего не меняю." >&2
  exit 1
fi
if [[ ! -c /dev/sil6250 ]]; then
  echo "/dev/sil6250 не найден; сначала проверь загруженный модуль. Ничего не меняю." >&2
  exit 1
fi
if [[ ! -f "$PLUGIN_SRC" ]]; then
  echo "Собранный драйвер не найден: $PLUGIN_SRC. Ничего не меняю." >&2
  exit 1
fi
if [[ -e "$PLUGIN_DST" || -e "$DROPIN" ]]; then
  echo "Целевой файл уже существует; не перезаписываю его:" >&2
  [[ ! -e "$PLUGIN_DST" ]] || echo "  $PLUGIN_DST" >&2
  [[ ! -e "$DROPIN" ]] || echo "  $DROPIN" >&2
  echo "Сохрани текущую конфигурацию и сообщи мне; ничего не меняю." >&2
  exit 1
fi

install -d -m 0755 "$TOD_DIR" "$DROPIN_DIR"
install -m 0644 "$PLUGIN_SRC" "$PLUGIN_DST"
cat > "$DROPIN" <<EOF
[Service]
Environment=FP_TOD_DRIVERS_DIR=$TOD_DIR
Environment=FP_SIL6250=/dev/sil6250
DeviceAllow=/dev/sil6250 rw
EOF
chmod 0644 "$DROPIN"
systemctl daemon-reload
systemctl try-restart fprintd.service || true

echo "Установлен только TOD-драйвер и drop-in для штатного fprintd."
echo "Пакеты, PAM, Secure Boot, Fingwit и парольный вход этим скриптом не изменялись."
echo "Плагин: $PLUGIN_DST"
echo "Конфигурация: $DROPIN"
echo "Проверь устройство командой: fprintd-list \"\$USER\""
