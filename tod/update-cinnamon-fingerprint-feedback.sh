#!/usr/bin/env bash
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Запусти через sudo: sudo bash $0" >&2
  exit 1
fi

python3 - <<'PY'
from datetime import datetime
from pathlib import Path
import os
import shutil
import stat
import tempfile

config = Path('/usr/share/cinnamon-screensaver/config.py')
source = Path('/usr/share/cinnamon-screensaver/unlock.py')
if not config.is_file() or not source.is_file():
    raise SystemExit('Не найдены файлы Cinnamon Screensaver; ничего не меняю.')
if 'VERSION="6.6.1"' not in config.read_text(encoding='utf-8'):
    raise SystemExit('Версия Cinnamon Screensaver отличается от ожидаемой 6.6.1; ничего не меняю.')

text = source.read_text(encoding='utf-8')
lines = text.splitlines(keepends=True)
marker = '    def on_authentication_info_changed(self, auth_client, info):'
starts = [i for i, line in enumerate(lines) if line.rstrip('\n') == marker]
if len(starts) != 1:
    raise SystemExit(f'Нашёл обработчиков auth-info: {len(starts)}; ничего не меняю.')
start = starts[0]
end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('    def ')), len(lines))
current = ''.join(lines[start:end])
if 'is_fingerprint_prompt' in current and 'Палец не распознан. Приложите его ещё раз' in current:
    print('Русские подсказки и обработка ошибки уже установлены в Cinnamon Screensaver.')
    print('Если блокировщик уже был открыт, выйди из сеанса и войди снова, затем проверь отпечаток.')
    raise SystemExit(0)
if 'self.authinfo_label' not in current or 'self.update_authinfo_label()' not in current:
    raise SystemExit('Обработчик отличается от ожидаемого; ничего не меняю.')

new = '''    def on_authentication_info_changed(self, auth_client, info):
        original_info = info.strip()
        normalized_info = original_info.lower()
        is_fingerprint_mismatch = (
            original_info in (
                "Failed to match fingerprint",
                "Failed to verify fingerprint",
                "Не удалось установить соответствие отпечатка",
                "Отпечаток не распознан",
            )
            or ("fingerprint" in normalized_info and ("fail" in normalized_info or "match" in normalized_info))
            or ("отпечат" in normalized_info and ("не удалось" in normalized_info or "не распознан" in normalized_info or "ошиб" in normalized_info))
        )
        is_fingerprint_prompt = (
            original_info == "Place your finger on the fingerprint reader"
            or "fingerprint reader" in normalized_info
            or "считывающее устройство" in normalized_info
            or "сканер отпечатков" in normalized_info
        )

        if is_fingerprint_mismatch:
            self.auth_info = "Палец не распознан. Приложите его ещё раз"
        elif is_fingerprint_prompt:
            self.auth_info = "Приложите палец к сканеру отпечатков"
        else:
            self.auth_info = info

        style = self.authinfo_label.get_style_context()
        if is_fingerprint_mismatch:
            style.add_class("auth-message")
        else:
            style.remove_class("auth-message")

        self.update_authinfo_label()

'''
updated = ''.join(lines[:start]) + new + ''.join(lines[end:])
compile(updated, str(source), 'exec')

stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
backup = source.with_name(source.name + '.sil6250-feedback-backup-' + stamp)
shutil.copy2(source, backup)
metadata = source.stat()
fd, temp_name = tempfile.mkstemp(prefix='.unlock.py.sil6250-feedback.', dir=str(source.parent))
try:
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        f.write(updated)
    os.chmod(temp_name, stat.S_IMODE(metadata.st_mode))
    os.chown(temp_name, metadata.st_uid, metadata.st_gid)
    os.replace(temp_name, source)
except Exception:
    try:
        os.unlink(temp_name)
    except FileNotFoundError:
        pass
    shutil.copy2(backup, source)
    raise

print('Обработчик обновлён: ошибка отпечатка будет показана по-русски красным текстом.')
print('При следующем запросе отпечатка цвет вернётся к обычному.')
print('PAM и парольный вход не менялись.')
print(f'Резервная копия: {backup}')
print('Выйди из сеанса Cinnamon и войди снова, чтобы применить изменение.')
PY
