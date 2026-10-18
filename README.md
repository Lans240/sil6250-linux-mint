# Silead SIL6250 fingerprint reader on Linux — Linux Mint and Huawei MateBook D16

Community-maintained Linux driver and installation guide for the Silead/Petaic SIL6250 fingerprint reader, with a tested Linux Mint Cinnamon setup for the Huawei MateBook D16. The repository contains the ACPI kernel module, the Rust protocol/matcher library, a libfprint TOD plugin, installation and rollback helpers, diagnostics, and a Russian guide.

**Русская подробная инструкция:** [README.ru.md](README.ru.md).

> **Linux Mint safety:** use the stock Mint `fprintd` and the `tod/` plugin in this repository. Do **not** install the Debian `open-fprintd` package on a Mint system using Fingwit: it declares conflicts with the stock `fprintd` and Fingwit/PAM packages and can remove or replace them. Do not run the `sil6250d` open-fprintd backend on Mint. The install script now refuses that backend when it detects the stock Debian/Ubuntu `fprintd` package.
>
> The documented installation path does not disable Secure Boot and does not edit PAM. Do not change PAM until enrollment and verification work.

## Tested setup

- Huawei MateBook D16 with an ACPI `SIL6250` device.
- Linux Mint Cinnamon, Secure Boot left enabled.
- Firmware query returned `13.32.0.0`; handshake passed; a 5,120-byte frame was captured; enrollment completed through `fprintd`; a registered finger verified successfully.
- The reader and firmware are not documented by the vendor. Other laptops that contain a SIL6250 may use different firmware, ACPI resources, or board wiring and are not confirmed by this report.

This is community reverse-engineered software, not an official Huawei or Silead driver. Fingerprint authentication is a convenience feature; keep a working password login and test the password fallback before relying on the reader.

## Choose the correct backend

There are two different ways to connect this hardware to fprintd:

1. **Recommended for Linux Mint / Debian systems with stock `fprintd`:** `kernel/` plus the `tod/` libfprint plugin. This keeps Mint's existing fprintd, Fingwit, PAM configuration, and password login.
2. **Alternative for distributions set up with `open-fprintd`:** the Rust `sil6250d` D-Bus daemon. It registers with the open-fprintd manager and is useful where that backend is already supported.

Do not install both backends. On Mint, follow only the TOD path below. In particular, do not install `open-fprintd` to make the TOD plugin work; it is not required by that path.

## Linux Mint installation

Commands below are run in a terminal. Commands beginning with `sudo` ask for the administrator password. The build steps run as your regular user.

### 1. Check that the machine has the target device

```sh
lscpu | head
cat /sys/bus/platform/devices/SIL6250:00/modalias 2>/dev/null || true
```

The modalias is expected to contain `SIL6250`. If the platform device is absent, stop here: this driver is specific to this ACPI device and cannot make an unrelated fingerprint reader appear.

### 2. Install build prerequisites

```sh
sudo apt update
sudo apt install build-essential dkms linux-headers-$(uname -r) mokutil pkg-config libssl-dev libfprint-2-tod-dev
```

Install a current stable Rust toolchain if the distro's `cargo`/`rustc` is too old for the locked dependencies. Use the official [Rust installation instructions](https://www.rust-lang.org/tools/install); do not run Cargo with `sudo`.

Clone the public repository and enter it:

```sh
git clone https://github.com/Lans240/sil6250-linux-mint.git
cd sil6250-linux-mint
```

### 3. Build and install the kernel module

```sh
./install.sh kernel
```

This stage builds/installs the DKMS kernel module and installs its udev rule. It does not install a fingerprint daemon, touch PAM, or disable Secure Boot. The installer refuses to overwrite an existing DKMS source directory; if you already installed an older copy, inspect `dkms status` before deciding how to update it.

Check that the module and character device exist:

```sh
mokutil --sb-state
modinfo -F filename sil6250
modinfo -F signer sil6250
lsmod | grep -w sil6250
ls -l /dev/sil6250
```

`/dev/sil6250` should be a character device. If `modinfo` cannot find the module, check that the headers for the running kernel are installed and that DKMS built the module for `uname -r`:

```sh
uname -r
dkms status
```

#### Secure Boot and MOK

Leave Secure Boot enabled. DKMS signs the out-of-tree module using a local Machine Owner Key (MOK). If `modprobe sil6250` reports that the key was rejected, check the kernel log and whether the DKMS public certificate is enrolled:

```sh
sudo modprobe sil6250
journalctl -k -b --no-pager | grep -iE 'sil6250|key was rejected|signature'
mokutil --list-enrolled
```

If DKMS created `/var/lib/dkms/mok.pub` and it is not enrolled, request enrollment:

```sh
sudo mokutil --import /var/lib/dkms/mok.pub
```

Set a one-time enrollment password, reboot, choose **Enroll MOK** in the blue MOK Manager screen, confirm the key, and reboot again. DKMS key paths vary by distribution; if that file does not exist, inspect the DKMS signing configuration and enroll the certificate that signed the module. Never enroll or publish a private `.key` file.

### 4. Build and install the TOD plugin for stock fprintd

Build the plugin as your regular user:

```sh
make -C tod/driver
```

Install only the plugin and its systemd service drop-in:

```sh
sudo bash tod/install-tod.sh
```

The script checks that the stock `fprintd`, Fingwit, and `/dev/sil6250` are present, refuses to overwrite existing destination files, and installs the TOD plugin into the directory reported by `pkg-config`. It does not install or remove packages and does not alter Secure Boot or PAM.

### 5. Confirm device detection, then enroll and verify

```sh
fprintd-list "$USER"
fprintd-enroll -f right-index-finger "$USER"
fprintd-verify "$USER"
```

Enrollment takes several separate presses. Lift the finger fully between prompts and reposition it to cover the center and edges. Do not press `Ctrl+C` while the enrollment stages are progressing. Enroll each desired finger separately. Verify each finger more than once and check that an unenrolled finger returns `verify-no-match`.

The GUI should now list the reader in Mint's **Fingerprints / Fingwit** settings. The lock screen and `sudo` use PAM. The stock password path remains available; do not edit PAM as a troubleshooting step for a device that has not yet enrolled and verified.

### 6. Optional: additional visual feedback in Cinnamon

If Cinnamon shows an English fingerprint prompt or gives no clear indication of a mismatch, the helper can translate the supported lock-screen prompt to Russian and show a mismatch in red. It makes a timestamped backup, checks the Cinnamon source version/handler, and refuses to patch an unknown source layout:

```sh
sudo bash tod/update-cinnamon-fingerprint-feedback.sh
```

Log out of Cinnamon and back in to reload the screensaver. Cinnamon package updates may replace the patched file. This optional customization changes the Cinnamon screensaver source; it does not change PAM or fprintd. Skip it if the Cinnamon version guard refuses the patch.

### 7. Optional: increase the fingerprint retry window in PAM

Only after `fprintd-enroll` and `fprintd-verify` work, you may run:

```sh
sudo bash tod/set-fingerprint-retries.sh
```

This helper is specific to the expected Mint/Fingwit PAM line. It creates a backup and refuses to change a different PAM stack. It changes the shared `common-auth` fingerprint retry settings (so it may affect the lock screen, `sudo`, and other PAM prompts using that profile). The password fallback line is preserved. Review the helper and its backup before use; never remove the password fallback.

## Diagnostics

Run these checks if the Fingerprints settings page says no device is available:

```sh
lsmod | grep -w sil6250
ls -l /dev/sil6250
pkg-config --variable=tod_driversdir libfprint-2-tod-1
systemctl status fprintd --no-pager
systemctl cat fprintd
journalctl -u fprintd -b --no-pager | tail -100
```

Check that `fprintd`'s systemd service has `FP_TOD_DRIVERS_DIR` set to the TOD directory and `FP_SIL6250=/dev/sil6250`. The plugin path should end in `libfprint-2-tod-sil6250.so`.

To diagnose the sensor protocol without storing a fingerprint image:

```sh
cargo build --locked --release -p sil6250 --example hsdiag
sudo ./target/release/examples/hsdiag /dev/sil6250 auto --quiet
cargo build --locked --release -p sil6250 --example fw_probe
sudo ./target/release/examples/fw_probe
```

The optional `capture_check` example reports frame size and a quality score but does not save the captured frame:

```sh
cargo build --locked --release -p sil6250 --example capture_check
sudo ./target/release/examples/capture_check
```

Do not attach raw captures, enrolled-print files, kernel dumps, or full system logs to public issues. Redact usernames, serial numbers, hostnames, and local paths from any diagnostic output before sharing it.

## Removal / rollback

Remove only the TOD plugin and its service drop-in:

```sh
sudo bash tod/remove-tod.sh
```

This restores fprintd to its previous driver set; enrolled templates in the fprintd data directory are not deleted by that helper. To remove a kernel module installed through this repository's DKMS config, inspect first:

```sh
dkms status
```

Then remove only the matching `sil6250/0.1.0` registration if you are sure it belongs to this installation:

```sh
sudo dkms remove sil6250/0.1.0 --all
```

Do not delete MOK keys: DKMS may share them with other modules. Do not disable Secure Boot or remove PAM password authentication to troubleshoot this driver.

## Alternative `sil6250d` backend

`sil6250d/` is a Rust D-Bus backend for `open-fprintd`, not the recommended Mint route. It is kept for distributions that already use open-fprintd. It must not be used alongside the TOD plugin for the same sensor. The `install.sh daemon` stage checks that `open-fprintd.service` exists and refuses to run when the stock Debian/Ubuntu `fprintd` package is installed.

## Repository contents

- `kernel/` — ACPI module and DKMS/udev integration.
- `sil6250/` — Rust transport, firmware protocol, image feature extractor and matcher.
- `tod/` — libfprint TOD driver, Rust/C bridge, Mint installation/removal/update helpers.
- `sil6250d/` — optional open-fprintd backend.
- `REVERSE_ENGINEERING_DETAILS.md` — protocol and implementation notes.

No fingerprint images, enrolled biometric templates, compiled `.so`/`.ko` files, user logs, local system paths, Git credentials, or original Git history are included. Generated binaries, build outputs, and raw image formats are ignored by Git.

## Credits and license

This is a community setup/maintenance repository based on the SIL6250 Linux driver work by [AlexDaichendt/sil6250-linux](https://github.com/AlexDaichendt/sil6250-linux). Thanks also to the [open-fprintd](https://github.com/uunicorn/open-fprintd) project and the [GXFP Linux driver](https://github.com/Void755/gxfp_linux_driver) for relevant interface/reference work. Their projects remain separate and retain their own licenses.

The source files carry SPDX identifiers; the root `LICENSE` is LGPL-2.1 and `LICENSES/GPL-2.0.txt` is included for GPL-2.0 components. See the file headers for the license of each component.
