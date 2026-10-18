# SIL6250 implementation notes

This file summarizes the public, non-secret architecture used by this community driver. It intentionally omits captured mailbox frames, firmware fallback keys, packet payloads containing key material, and proprietary binary extracts.

## Device discovery and kernel interface

On the tested Huawei MateBook D16, ACPI exposes a platform device with HID `SIL6250`. Its resources include a small memory window, a host interrupt, and GPIO strobes used to coordinate transactions with the embedded controller. The kernel module binds to that ACPI device and exposes `/dev/sil6250`; userspace owns protocol handling and image processing.

The exact ACPI wiring and firmware are device-specific. A different computer with a similarly named sensor may use different resources or firmware and needs independent validation.

## Userspace protocol flow

The Rust library performs the device prelude, exchanges framed mailbox requests, and provisions a fresh random 32-byte TLS-PSK with command `0x27` before each TLS handshake. The key is generated at runtime; the repository does not contain static firmware keys or captured PSK blobs. The `hsdiag` example reports whether provisioning and the handshake succeeded.

For capture, userspace polls for a finger, arms a capture, and reads the resulting data over the established TLS session. On the tested unit the returned frame is 64 × 80 pixels (5,120 bytes). The diagnostic `capture_check` reports frame size and a quality score without writing the raw frame to disk.

## Kernel and fingerprint services

The kernel module is a transport broker. Finger enrollment, templates, and matching remain in userspace. On Linux Mint, the tested integration uses the distribution's stock `fprintd` with a libfprint TOD plugin. It does not require replacing Mint's service with `open-fprintd`, editing PAM, or disabling Secure Boot.

## Reproduction and reporting

The README contains the supported installation path, prerequisites, verification commands, and rollback instructions. For public bug reports, include only the minimum relevant command output. Do not attach fingerprint images, enrolled templates, raw mailbox dumps, TLS traces, or diagnostic logs that may contain transient session material. Redact account names, hostnames, serial numbers, and local paths.
