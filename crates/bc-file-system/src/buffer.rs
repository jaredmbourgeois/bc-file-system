//! The buffer protocol of the header ("ANSWERS OF VARIABLE LENGTH"), absorbed so a consumer never sees
//! `BufferTooSmall`: the caller offers a buffer, and a call whose answer does not fit fails and names the
//! capacity it needs. Two readings, the C++ and Swift faces' own:
//!
//! - **the grow loop**, for an engine answer, which can change between two calls (a file grows; a symlink is
//!   replaced; a condemn or a temporary directory retries with a longer name): a generous first buffer, then the
//!   capacity the core named, at most three attempts in all, then the error as received, so a file that keeps
//!   growing cannot livelock the face. A text answer's buffer is a slice of blanks; a file's bytes and a
//!   listing's entries go into a vector's spare capacity instead, never initialised first, because the core
//!   writes what it answers and their first offers are large (1 MiB, 1,024 entries);
//! - **two calls**, for a path module answer, which is a pure function of its arguments and cannot change: the
//!   size first, then exactly that much.
//!
//! None calls into the core itself: each takes the call as a closure with everything but the buffer bound, so
//! the live module keeps every `unsafe` call (and the one `set_len` that makes a vector's length cover what the
//! core wrote) and these loops are tested without a core.

use crate::error::ErrorKind;
use crate::ffi;
use crate::options::ReadLimit;

/// The first capacity offered for a file's bytes: 1 MiB, so one call serves every file up to 1 MiB; a larger one
/// costs one more call, since the core names the capacity it needs, and that call resolves, opens and measures
/// again. The answer is read into its own spare capacity, never zero-filled first. Measured at stage 28 through
/// the core (macOS, APFS, median µs, one-minute load 57 to 95), against a 64 KiB first offer: a 4 KiB file 12.96
/// against 12.92, a 64 KiB file 17.12 against 15.88 (the shrink to its size), a 256 KiB file 28.08 against 38.50,
/// a 1 MiB file 43.04 against 59.83; the 64 KiB offer's zero fill had cost 0.71 µs more at 4 KiB and 7.88 µs more
/// at 1 MiB. A larger first offer is a fresh mapping on every call on macOS: 16 MiB made a 1 MiB read 68.46
/// against 43.04.
pub(crate) const FIRST_READ_CAPACITY: usize = 1024 * 1024;
/// The first capacity offered for a text answer (a symlink's text, a condemned or temporary name, a directory's
/// opened-as text).
pub(crate) const FIRST_TEXT_CAPACITY: usize = 1024;
/// The first number of entries offered for a directory listing: 1,024 (264 KiB), never initialised first.
/// Measured at stage 28 through the core, with the core reading the directory once (same probe, median µs): an
/// empty directory 18.12 against 18.21 for 64 entries, 10 entries 25.88 against 25.92, 1,000 entries 907.67
/// against 1,703.62 (64 and 256 entries both overflow there and pay a second call). Zero-filled, the 1,024-entry
/// offer cost 2.3 µs more on every listing.
pub(crate) const FIRST_ENTRY_CAPACITY: usize = 1024;
/// The grow loop's bound: after this many attempts a `BufferTooSmall` is returned as received.
pub(crate) const MOST_ATTEMPTS: u32 = 3;

/// The first capacity offered for a read under `limit`: under a bound, no successful answer is longer than the
/// bound, the size the caller accepts, so a bound up to [`FIRST_READ_CAPACITY`] is offered whole and its read is
/// one call. A larger bound is offered [`FIRST_READ_CAPACITY`], as `WholeFile` is: an offer past it would be a
/// fresh mapping on every call.
pub(crate) fn first_read_capacity(limit: ReadLimit) -> usize {
    match limit {
        ReadLimit::WholeFile => FIRST_READ_CAPACITY,
        ReadLimit::AtMost(bound) => match usize::try_from(bound) {
            Ok(bound) => bound.min(FIRST_READ_CAPACITY),
            // A bound past `usize` is past the first capacity too.
            Err(_) => FIRST_READ_CAPACITY,
        },
    }
}

/// The grow loop: offers `first_capacity` elements (each `blank` until the core writes it), then the capacity the
/// core named, at most [`MOST_ATTEMPTS`] times. `call` is one call of the protocol: the core's function with
/// everything but the buffer bound, handed the buffer (its length is the capacity), the length to fill and the
/// error to fill, answering the core's status. The answer comes back holding exactly what the core stored; a
/// failure comes back as the core's error struct (boxed, so the `Result` stays small), for the caller to convert
/// with the location it holds.
pub(crate) fn grown_answer<T: Copy>(
    first_capacity: usize,
    blank: T,
    mut call: impl FnMut(&mut [T], &mut usize, &mut ffi::bc_file_system_error) -> i32,
) -> Result<Vec<T>, Box<ffi::bc_file_system_error>> {
    let mut capacity = first_capacity;
    let mut attempt = 1;
    loop {
        let mut answer = vec![blank; capacity];
        let mut length = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let status = call(&mut answer, &mut length, &mut c_error);
        if status == 0 {
            answer.truncate(length);
            return Ok(answer);
        }
        if status == i32::from(ErrorKind::BufferTooSmall) && attempt < MOST_ATTEMPTS {
            capacity = length;
            attempt += 1;
            continue;
        }
        return Err(Box::new(c_error));
    }
}

/// The grow loop over a buffer the core writes in place: a fresh vector with room for `first_capacity` elements,
/// never initialised first, then for the capacity the core named, at most [`MOST_ATTEMPTS`] times. `call` is one
/// call of the protocol: it is handed an empty vector with room for at least the capacity it is handed beside it,
/// the length to fill and the error to fill; it offers the core exactly that capacity of the vector's spare room
/// and, on success, leaves the vector holding exactly the elements the core stored, which only `call` can do
/// (the length covering what the core wrote is `unsafe`, so it lives in the live module). It answers the core's
/// status. A failure comes back as the core's error struct, boxed, as for [`grown_answer`].
pub(crate) fn grown_in_place<T>(
    first_capacity: usize,
    mut call: impl FnMut(&mut Vec<T>, usize, &mut usize, &mut ffi::bc_file_system_error) -> i32,
) -> Result<Vec<T>, Box<ffi::bc_file_system_error>> {
    let mut capacity = first_capacity;
    let mut attempt = 1;
    loop {
        let mut answer = Vec::with_capacity(capacity);
        let mut length = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let status = call(&mut answer, capacity, &mut length, &mut c_error);
        if status == 0 {
            return Ok(answer);
        }
        if status == i32::from(ErrorKind::BufferTooSmall) && attempt < MOST_ATTEMPTS {
            capacity = length;
            attempt += 1;
            continue;
        }
        return Err(Box::new(c_error));
    }
}

/// Two calls: the size first (an empty buffer, the header's stated way to ask), then exactly that many bytes.
/// An answer that is empty comes back from the first call. `call` is as for [`grown_answer`].
pub(crate) fn measured_answer(
    mut call: impl FnMut(&mut [u8], &mut usize, &mut ffi::bc_file_system_error) -> i32,
) -> Result<Vec<u8>, Box<ffi::bc_file_system_error>> {
    let mut needed = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    let sized = call(&mut [], &mut needed, &mut c_error);
    if sized == 0 {
        return Ok(Vec::new());
    }
    if sized != i32::from(ErrorKind::BufferTooSmall) {
        return Err(Box::new(c_error));
    }
    let mut answer = vec![0; needed];
    let mut length = 0;
    let status = call(&mut answer, &mut length, &mut c_error);
    if status != 0 {
        return Err(Box::new(c_error));
    }
    answer.truncate(length);
    Ok(answer)
}

#[cfg(test)]
mod tests {
    use super::{MOST_ATTEMPTS, grown_answer, grown_in_place, measured_answer};
    use crate::error::ErrorKind;
    use crate::ffi;

    /// A core stand-in for the protocol: each call is handed the next of `answers` (the length the answer has at
    /// that moment, so a test can make it grow between calls) and records the capacity it was offered.
    struct ScriptedCore {
        answers: Vec<usize>,
        capacities_offered: Vec<usize>,
    }

    impl ScriptedCore {
        fn new_answering(answers: &[usize]) -> Self {
            Self {
                answers: answers.to_vec(),
                capacities_offered: Vec::new(),
            }
        }

        /// One call: stores `b'x'` bytes when the answer fits, else names the capacity it needs.
        fn call(
            &mut self,
            buffer: &mut [u8],
            length: &mut usize,
            c_error: &mut ffi::bc_file_system_error,
        ) -> i32 {
            let answer_length = self.answers[self.capacities_offered.len()];
            self.capacities_offered.push(buffer.len());
            *length = answer_length;
            if answer_length > buffer.len() {
                c_error.kind = i32::from(ErrorKind::BufferTooSmall);
                return c_error.kind;
            }
            buffer[..answer_length].fill(b'x');
            0
        }

        /// One call in place: appends `b'x'` bytes to the empty vector when the answer fits the capacity
        /// offered (safely, as a stand-in for the live module's writing through the spare room), else names the
        /// capacity it needs and leaves the vector empty.
        fn call_in_place(
            &mut self,
            answer: &mut Vec<u8>,
            capacity: usize,
            length: &mut usize,
            c_error: &mut ffi::bc_file_system_error,
        ) -> i32 {
            assert!(
                answer.is_empty(),
                "every attempt starts from an empty vector"
            );
            assert!(
                answer.capacity() >= capacity,
                "the vector has room for what is offered"
            );
            let answer_length = self.answers[self.capacities_offered.len()];
            self.capacities_offered.push(capacity);
            *length = answer_length;
            if answer_length > capacity {
                c_error.kind = i32::from(ErrorKind::BufferTooSmall);
                return c_error.kind;
            }
            answer.resize(answer_length, b'x');
            0
        }
    }

    #[test]
    fn the_loop_in_place_answers_on_the_first_attempt_when_the_answer_fits() {
        let mut core = ScriptedCore::new_answering(&[5]);

        let answer = grown_in_place(
            8,
            |answer: &mut Vec<u8>, capacity: usize, length: &mut usize, c_error: &mut _| {
                core.call_in_place(answer, capacity, length, c_error)
            },
        );

        assert_eq!(answer.ok(), Some(b"xxxxx".to_vec()));
        assert_eq!(core.capacities_offered, [8]);
    }

    #[test]
    fn the_loop_in_place_offers_the_capacity_the_core_named_then_stops_after_three_attempts() {
        let mut grows_once = ScriptedCore::new_answering(&[20, 30, 30]);
        let mut keeps_growing = ScriptedCore::new_answering(&[20, 30, 40, 40]);

        let grown = grown_in_place(
            8,
            |answer: &mut Vec<u8>, capacity: usize, length: &mut usize, c_error: &mut _| {
                grows_once.call_in_place(answer, capacity, length, c_error)
            },
        );
        let refused = grown_in_place(
            8,
            |answer: &mut Vec<u8>, capacity: usize, length: &mut usize, c_error: &mut _| {
                keeps_growing.call_in_place(answer, capacity, length, c_error)
            },
        );

        assert_eq!(grown.map(|bytes| bytes.len()).ok(), Some(30));
        assert_eq!(grows_once.capacities_offered, [8, 20, 30]);
        let c_error = refused.expect_err("a file that keeps growing ends the loop");
        assert_eq!(c_error.kind, i32::from(ErrorKind::BufferTooSmall));
        assert_eq!(keeps_growing.capacities_offered, [8, 20, 30]);
    }

    #[test]
    fn the_grow_loop_answers_on_the_first_attempt_when_the_answer_fits() {
        let mut core = ScriptedCore::new_answering(&[5]);

        let answer = grown_answer(
            8,
            0,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut _| {
                core.call(buffer, length, c_error)
            },
        );

        assert_eq!(answer.ok(), Some(b"xxxxx".to_vec()));
        assert_eq!(core.capacities_offered, [8]);
    }

    #[test]
    fn the_grow_loop_offers_the_capacity_the_core_named_on_the_second_attempt() {
        let mut core = ScriptedCore::new_answering(&[20, 20]);

        let answer = grown_answer(
            8,
            0,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut _| {
                core.call(buffer, length, c_error)
            },
        );

        assert_eq!(answer.map(|bytes| bytes.len()).ok(), Some(20));
        assert_eq!(core.capacities_offered, [8, 20]);
    }

    #[test]
    fn the_grow_loop_answers_on_the_third_attempt_after_the_answer_grew_once() {
        let mut core = ScriptedCore::new_answering(&[20, 30, 30]);

        let answer = grown_answer(
            8,
            0,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut _| {
                core.call(buffer, length, c_error)
            },
        );

        assert_eq!(answer.map(|bytes| bytes.len()).ok(), Some(30));
        assert_eq!(core.capacities_offered, [8, 20, 30]);
    }

    #[test]
    fn the_grow_loop_returns_the_error_as_received_where_a_fourth_attempt_would_be_needed() {
        let mut core = ScriptedCore::new_answering(&[20, 30, 40, 40]);

        let answer = grown_answer(
            8,
            0,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut _| {
                core.call(buffer, length, c_error)
            },
        );

        let c_error = answer.expect_err("a file that keeps growing ends the loop");
        assert_eq!(c_error.kind, i32::from(ErrorKind::BufferTooSmall));
        assert_eq!(
            core.capacities_offered.len(),
            usize::try_from(MOST_ATTEMPTS).expect("three fits")
        );
        assert_eq!(core.capacities_offered, [8, 20, 30]);
    }

    #[test]
    fn the_grow_loop_passes_any_other_error_through_at_once() {
        let mut calls = 0;

        let answer = grown_answer(
            8,
            0_u8,
            |_buffer: &mut [u8], _length: &mut usize, c_error: &mut _| {
                calls += 1;
                c_error.kind = i32::from(ErrorKind::NotFound);
                c_error.kind
            },
        );

        assert_eq!(
            answer.map_err(|c_error| c_error.kind),
            Err(i32::from(ErrorKind::NotFound))
        );
        assert_eq!(calls, 1);
    }

    #[test]
    fn two_calls_ask_the_size_with_an_empty_buffer_then_offer_exactly_that() {
        let mut core = ScriptedCore::new_answering(&[12, 12]);

        let answer = measured_answer(|buffer: &mut [u8], length: &mut usize, c_error: &mut _| {
            core.call(buffer, length, c_error)
        });

        assert_eq!(answer.map(|bytes| bytes.len()).ok(), Some(12));
        assert_eq!(core.capacities_offered, [0, 12]);
    }

    #[test]
    fn two_calls_answer_an_empty_answer_from_the_first() {
        let mut core = ScriptedCore::new_answering(&[0]);

        let answer = measured_answer(|buffer: &mut [u8], length: &mut usize, c_error: &mut _| {
            core.call(buffer, length, c_error)
        });

        assert_eq!(answer.ok(), Some(Vec::new()));
        assert_eq!(core.capacities_offered, [0]);
    }
}
