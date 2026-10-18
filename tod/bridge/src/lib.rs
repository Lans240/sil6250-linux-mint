//! Narrow C ABI for the libfprint TOD driver.
//!
//! Captured images stay in memory. The only bytes returned to the caller are
//! the project's serialized feature descriptors, suitable for FpPrint::fpi-data.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::io;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use sil6250::{Engine, Features, Frame};

const QUALITY_MIN: f32 = 0.52;
const ENROLL_MAX_NCC: f32 = 0.85;
const MAX_SHIFT: i32 = 14;
const MIN_OVERLAP: i32 = 1200;

struct Session {
    engine: Engine,
    enrolled_frames: Vec<Frame>,
}

/// Open the kernel device, but defer its TLS handshake until first capture.
/// Returns null on failure. `path` must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn sil6250_session_new(path: *const c_char) -> *mut c_void {
    if path.is_null() {
        return ptr::null_mut();
    }
    catch_unwind(AssertUnwindSafe(|| {
        let path = CStr::from_ptr(path).to_str().ok()?;
        let engine = Engine::open(path).ok()?;
        Some(Box::into_raw(Box::new(Session {
            engine,
            enrolled_frames: Vec::new(),
        })) as *mut c_void)
    }))
    .ok()
    .flatten()
    .unwrap_or(ptr::null_mut())
}

/// Destroy a session returned by sil6250_session_new().
#[no_mangle]
pub unsafe extern "C" fn sil6250_session_free(session: *mut c_void) {
    if !session.is_null() {
        drop(Box::from_raw(session as *mut Session));
    }
}

/// Capture one usable frame and return its serialized Features blob.
///
/// `enroll_mode` enables the same quality and near-duplicate screening used by
/// sil6250d. Return values: 0 accepted; 1 retry (no finger/poor quality); 2
/// redundant press; -1 device or serialization error. Returned storage must be
/// released with sil6250_bytes_free().
#[no_mangle]
pub unsafe extern "C" fn sil6250_capture_features(
    session: *mut c_void,
    finger_timeout_ms: u32,
    enroll_mode: bool,
    out_data: *mut *mut u8,
    out_len: *mut usize,
) -> c_int {
    if session.is_null() || out_data.is_null() || out_len.is_null() {
        return -1;
    }
    *out_data = ptr::null_mut();
    *out_len = 0;

    match catch_unwind(AssertUnwindSafe(|| {
        let session = &mut *(session as *mut Session);
        let raw = match session.engine.capture_frame(finger_timeout_ms) {
            Ok(raw) => raw,
            Err(e) if e.kind() == io::ErrorKind::TimedOut => return 1,
            Err(_) => return -1,
        };
        let frame = Frame::from_raw(&raw);
        if frame.quality() < QUALITY_MIN {
            return 1;
        }
        if enroll_mode
            && session.enrolled_frames.iter().any(|previous| {
                frame.ncc_vs(previous, MAX_SHIFT, MIN_OVERLAP) > ENROLL_MAX_NCC
            })
        {
            return 2;
        }

        let features = Features::extract(&frame).serialize();
        if features.len() < 12 || features.len() > 128 * 1024 {
            return -1;
        }

        if enroll_mode {
            session.enrolled_frames.push(frame);
        }
        let boxed = features.into_boxed_slice();
        *out_len = boxed.len();
        *out_data = Box::into_raw(boxed) as *mut u8;
        0
    })) {
        Ok(status) => status,
        Err(_) => -1,
    }
}

/// Wait for the user to lift the finger between enrollment stages.
#[no_mangle]
pub unsafe extern "C" fn sil6250_wait_finger_up(
    session: *mut c_void,
    timeout_ms: u32,
) -> bool {
    if session.is_null() {
        return false;
    }
    catch_unwind(AssertUnwindSafe(|| {
        (*(session as *mut Session))
            .engine
            .wait_finger_up(timeout_ms)
    }))
    .unwrap_or(false)
}

/// Compare a fresh feature blob with one enrolled blob. Returns the project's
/// inlier score, or -1 if either blob is malformed.
#[no_mangle]
pub unsafe extern "C" fn sil6250_match_features(
    probe: *const u8,
    probe_len: usize,
    enrolled: *const u8,
    enrolled_len: usize,
) -> c_int {
    if probe.is_null() || enrolled.is_null() || probe_len > 128 * 1024 || enrolled_len > 128 * 1024 {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let probe = Features::deserialize(std::slice::from_raw_parts(probe, probe_len))?;
        let enrolled = Features::deserialize(std::slice::from_raw_parts(enrolled, enrolled_len))?;
        Some(probe.verify_against(std::slice::from_ref(&enrolled)))
    }))
    .ok()
    .flatten()
    .unwrap_or(-1)
}

/// Release a byte buffer returned by sil6250_capture_features().
#[no_mangle]
pub unsafe extern "C" fn sil6250_bytes_free(data: *mut u8, len: usize) {
    if !data.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, len)));
    }
}
