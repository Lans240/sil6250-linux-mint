// SPDX-License-Identifier: GPL-2.0
//! PSK provisioning (cmd 0x27): key blob wrap/unwrap, key-id, wire helpers.
//!
//! The host generates a fresh 32-byte PSK and sends it to the sensor inside a
//! 128-byte blob (cmd 0x27) before each TLS handshake. No built-in firmware
//! fallback keys or captured key blobs are included in this project.
//!
//! Once keyed, the sensor presents PSK identity `SHA384(key)[..32]` and answers
//! cmd 0x28 with `SHA384(key)` followed by 16 zero bytes. An unkeyed sensor
//! returns all zeros for both.

use std::io;

use crate::proto::{PETAIT_CMD_KEY_ID, PETAIT_CMD_SET_PSK};

pub const PSK_LEN: usize = 32;
pub const PSK_SALT_LEN: usize = 8;
pub const PSK_BLOB_LEN: usize = 128;
/// cmd 0x28 response length.
pub const KEY_ID_LEN: usize = 64;
/// PSK identity length on the TLS wire.
pub const IDENTITY_LEN: usize = 32;

const SALT_XOR: u8 = 0x86;
const KEY_XOR: u8 = 0x39;
const KEY_OFF: usize = 0x40;

// cmd 0x27 framing. Like the TLS record writes, the 128-byte blob starts in the
// low byte of the BE32 field and the length rides in the access-width byte.
pub const SET_PSK_DIR: u8 = 0x00;
pub const SET_PSK_WIDTH: u8 = PSK_BLOB_LEN as u8;
pub const SET_PSK_OUTER: u8 = 0x8f;
const SET_PSK_PAYLOAD: usize = PSK_BLOB_LEN - 1 + 4;

const RX_OFF: usize = 7; // data window offset in raw mailbox reads
const OP_ACK: u8 = 0x06;
const ACK_OK: u8 = 0xaa;

pub fn wrap(
    salt: &[u8; PSK_SALT_LEN],
    key: &[u8; PSK_LEN],
    filler: [u8; PSK_BLOB_LEN],
) -> [u8; PSK_BLOB_LEN] {
    let mut out = filler;
    for i in 0..PSK_SALT_LEN {
        out[1 + 2 * i] = salt[i] ^ SALT_XOR;
    }
    for i in 0..PSK_LEN {
        out[KEY_OFF + i] = key[i] ^ salt[i % PSK_SALT_LEN] ^ KEY_XOR;
    }
    out
}

/// Inverse of [`wrap`]: recover `(salt, key)` from a blob.
pub fn unwrap(blob: &[u8; PSK_BLOB_LEN]) -> ([u8; PSK_SALT_LEN], [u8; PSK_LEN]) {
    let mut salt = [0u8; PSK_SALT_LEN];
    for i in 0..PSK_SALT_LEN {
        salt[i] = blob[1 + 2 * i] ^ SALT_XOR;
    }
    let mut key = [0u8; PSK_LEN];
    for i in 0..PSK_LEN {
        key[i] = blob[KEY_OFF + i] ^ KEY_XOR ^ salt[i % PSK_SALT_LEN];
    }
    (salt, key)
}

/// Fresh random key in a fresh random blob (OpenSSL CSPRNG).
pub fn random_blob() -> io::Result<([u8; PSK_LEN], [u8; PSK_BLOB_LEN])> {
    let mut key = [0u8; PSK_LEN];
    let mut salt = [0u8; PSK_SALT_LEN];
    let mut filler = [0u8; PSK_BLOB_LEN];
    for buf in [&mut key[..], &mut salt[..], &mut filler[..]] {
        openssl::rand::rand_bytes(buf).map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;
    }
    Ok((key, wrap(&salt, &key, filler)))
}

/// `SHA384(key)`: what a keyed sensor reports as its key id.
pub fn key_id(key: &[u8; PSK_LEN]) -> [u8; 48] {
    openssl::sha::sha384(key)
}

/// The PSK identity a sensor keyed with `key` presents: `SHA384(key)[..32]`.
pub fn identity(key: &[u8; PSK_LEN]) -> [u8; IDENTITY_LEN] {
    let mut id = [0u8; IDENTITY_LEN];
    id.copy_from_slice(&key_id(key)[..IDENTITY_LEN]);
    id
}

/// The cmd 0x28 data a sensor keyed with `key` returns: key id + 16 zeros.
pub fn key_id_data(key: &[u8; PSK_LEN]) -> [u8; KEY_ID_LEN] {
    let mut out = [0u8; KEY_ID_LEN];
    out[..48].copy_from_slice(&key_id(key));
    out
}

/// `(be32, payload)` arguments that make `xfer_raw` emit the cmd 0x27 frame.
pub fn set_psk_args(blob: &[u8; PSK_BLOB_LEN]) -> (u32, [u8; SET_PSK_PAYLOAD]) {
    let mut payload = [0u8; SET_PSK_PAYLOAD];
    payload[..PSK_BLOB_LEN - 1].copy_from_slice(&blob[1..]);
    (blob[0] as u32, payload)
}

/// True if a raw mailbox read is the sensor's "key accepted" answer to 0x27.
pub fn is_set_psk_ack(rx: &[u8]) -> bool {
    rx.len() > RX_OFF
        && rx[0] == 0x5a
        && rx[1] == PETAIT_CMD_SET_PSK
        && rx[2] == OP_ACK
        && rx[RX_OFF] == ACK_OK
}

/// The 64 data bytes of a raw cmd 0x28 mailbox read.
pub fn key_id_from_response(rx: &[u8]) -> Option<&[u8]> {
    if rx.len() < RX_OFF + KEY_ID_LEN || rx[0] != 0x5a || rx[1] != PETAIT_CMD_KEY_ID {
        return None;
    }
    Some(&rx[RX_OFF..RX_OFF + KEY_ID_LEN])
}

/// Compare the identity OpenSSL hands the PSK callback against the expected
/// one. OpenSSL treats the identity as a C string, so one containing a zero
/// byte arrives cut short; only the surviving prefix can be compared there.
pub fn cstr_identity_matches(seen: &[u8], expected: &[u8; IDENTITY_LEN]) -> bool {
    let n = expected.iter().position(|&b| b == 0).unwrap_or(IDENTITY_LEN);
    seen == &expected[..n]
}

/// Pull the PSK identity out of the sensor's side of a TLS 1.2 handshake
/// (`stream` = the concatenated sensor→host bytes): the ClientKeyExchange
/// body, untouched by the C-string truncation of the PSK callback.
pub fn client_psk_identity(stream: &[u8]) -> Option<Vec<u8>> {
    // Reassemble the plaintext handshake messages (everything before CCS).
    let mut hs = Vec::new();
    let mut p = stream;
    while p.len() >= 5 {
        let len = ((p[3] as usize) << 8) | p[4] as usize;
        if p[0] != 0x16 || p.len() < 5 + len {
            break;
        }
        hs.extend_from_slice(&p[5..5 + len]);
        p = &p[5 + len..];
    }
    let mut m = &hs[..];
    while m.len() >= 4 {
        let len = ((m[1] as usize) << 16) | ((m[2] as usize) << 8) | m[3] as usize;
        if m.len() < 4 + len {
            break;
        }
        let body = &m[4..4 + len];
        if m[0] == 0x10 {
            // ClientKeyExchange, plain PSK: opaque psk_identity<0..2^16-1>
            if body.len() < 2 {
                return None;
            }
            let id_len = ((body[0] as usize) << 8) | body[1] as usize;
            return body.get(2..2 + id_len).map(|id| id.to_vec());
        }
        m = &m[4 + len..];
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn random_blob_round_trips() {
        let (key, blob) = random_blob().unwrap();
        assert_eq!(unwrap(&blob).1, key);
    }

    #[test]
    fn key_id_shapes() {
        let (key, _) = random_blob().unwrap();
        let id = identity(&key);
        let data = key_id_data(&key);
        assert_eq!(&data[..48], &key_id(&key));
        assert_eq!(&data[..IDENTITY_LEN], &id);
        assert_eq!(&data[48..], &[0u8; 16]);
    }

    #[test]
    fn set_psk_ack() {
        let ack = [0x5a, 0x27, 0x06, 0x01, 0x00, 0x00, 0x00, 0xaa, 0x00, 0x00, 0x00, 0x00, 0xcd, 0xfe, 0xff, 0xff];
        assert!(is_set_psk_ack(&ack));
        let mut bad = ack;
        bad[7] = 0x55;
        assert!(!is_set_psk_ack(&bad));
    }
}
