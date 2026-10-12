//! The buffer protocol of the header ("ANSWERS OF VARIABLE LENGTH"), absorbed so a consumer never sees
//! `BufferTooSmall`: the caller offers a buffer, and a call whose answer does not fit fails and names the
//! capacity it needs. Two readings, the C++ and Swift faces' own:
//!
//! - **the grow loop**, for an engine answer, which can change between two calls (a file grows; a symlink is
//!   replaced; a condemn or a temporary directory retries with a longer name): a generous first buffer, then the
//!   capacity the core named, at most three attempts in all, then the error as received, so a file that keeps
//!   growing cannot livelock the face;
//! - **two calls**, for a path module answer, which is a pure function of its arguments and cannot change: the
//!   size first, then exactly that much.
//!
//! Neither calls into the core itself: each takes the call as a closure with everything but the buffer bound,
//! so the live module keeps every `unsafe` call and these loops are tested without a core.

use crate::error::ErrorKind;
use crate::ffi;
use crate::options::ReadLimit;

/// The first capacity offered for a file's bytes: one read serves most files outright, and a larger one costs
/// one more call, since the core names the capacity it needs.
pub(crate) const FIRST_READ_CAPACITY: usize = 64 * 1024;
/// The first capacity offered for a text answer (a symlink's text, a condemned or temporary name, a directory's
/// opened-as text).
pub(crate) const FIRST_TEXT_CAPACITY: usize = 1024;
/// The first number of entries offered for a directory listing.
pub(crate) const FIRST_ENTRY_CAPACITY: usize = 64;
/// The grow loop's bound: after this many attempts a `BufferTooSmall` is returned as received.
pub(crate) const MOST_ATTEMPTS: u32 = 3;

/// The first capacity offered for a read under `limit`: under a bound, no successful answer is longer than the
/// bound, so a bound smaller than [`FIRST_READ_CAPACITY`] is offered whole.
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
    use super::{MOST_ATTEMPTS, grown_answer, measured_answer};
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
