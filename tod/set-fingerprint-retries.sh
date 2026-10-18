#!/usr/bin/env bash
set -euo pipefail

PAM_FILE=/etc/pam.d/common-auth
OLD='pam_fprintd.so max-tries=1 timeout=15'
NEW='pam_fprintd.so max-tries=3 timeout=30'
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$PAM_FILE.sil6250-backup-$STAMP"

if [[ $EUID -ne 0 ]]; then
  echo "Запусти через sudo: sudo bash $0" >&2
  exit 1
fi
if [[ ! -f "$PAM_FILE" ]] || ! grep -Fq 'pam_fingwit.so' "$PAM_FILE" || \
   ! grep -Fq "$OLD" "$PAM_FILE" || \
   ! grep -Fq 'pam_unix.so' "$PAM_FILE" || \
   ! grep -Fq 'pam_deny.so' "$PAM_FILE"; then
  echo "Стек PAM отличается от ожидаемого; ничего не меняю." >&2
  exit 1
fi
if [[ "$(grep -Fc "$OLD" "$PAM_FILE")" -ne 1 ]]; then
  echo "Неожиданное количество строк pam_fprintd; ничего не меняю." >&2
  exit 1
fi

TEMP="$(mktemp /etc/pam.d/.common-auth.sil6250.XXXXXX)"
trap 'rm -f -- "$TEMP"' EXIT
cp -a -- "$PAM_FILE" "$BACKUP"
sed "s/$OLD/$NEW/" "$PAM_FILE" > "$TEMP"
chown root:root "$TEMP"
chmod 0644 "$TEMP"
mv -f -- "$TEMP" "$PAM_FILE"
trap - EXIT

echo "Для Fingwit включены до 3 попыток и ожидание до 30 секунд."
echo "Парольный fallback PAM сохранён."
echo "Резервная копия исходного файла: $BACKUP"
echo "Область действия: PAM-запросы, использующие общий профиль Fingwit (экран блокировки, sudo и системные запросы)."
echo "Изменение вступает в силу для следующего PAM-запроса; перезагрузка не нужна."
