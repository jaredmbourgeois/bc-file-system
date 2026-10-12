//! The face's compositions, which every face carries (design doc, "Faces and their compositions"): absence as
//! `None` for a read, and line iteration over a read buffer. The third, the grow loop, is the buffer module's.
//! They are the only logic the face holds; every operation is otherwise one call into the core.

use crate::error::{Error, ErrorKind};
use crate::live::Engine;
use crate::location::Location;
use crate::options::ReadOptions;

impl Engine {
    /// [`Engine::read_file`], with absence as `None` instead of `NotFound`; every other error passes through as
    /// received, so a file that cannot be read is never mistaken for one that is not there.
    ///
    /// # Errors
    ///
    /// Every error of [`Engine::read_file`] but `NotFound`.
    pub fn read_file_if_present(
        &self,
        location: &Location<'_>,
        options: &ReadOptions,
    ) -> Result<Option<Vec<u8>>, Error> {
        match self.read_file(location, options) {
            Ok(bytes) => Ok(Some(bytes)),
            Err(error) if error.kind == ErrorKind::NotFound => Ok(None),
            Err(error) => Err(error),
        }
    }
}

/// The lines of a read buffer, each a view into it without its newline: split on `\n` only, the bytes untouched
/// (a `\r` stays where it was; the library never decides what text the bytes are). A final line without a
/// newline is a line; a trailing newline does not add an empty one; an empty buffer has no lines.
#[must_use]
pub fn lines(buffer: &[u8]) -> Lines<'_> {
    Lines { remaining: buffer }
}

/// The iterator [`lines`] answers.
#[derive(Clone, Debug)]
pub struct Lines<'buffer> {
    /// What is left after the lines already answered.
    remaining: &'buffer [u8],
}

impl<'buffer> Iterator for Lines<'buffer> {
    type Item = &'buffer [u8];

    fn next(&mut self) -> Option<&'buffer [u8]> {
        if self.remaining.is_empty() {
            return None;
        }
        // The line ends at the next newline, which neither side keeps; the last line may have none.
        let (line, rest): (&'buffer [u8], &'buffer [u8]) =
            match self.remaining.iter().position(|byte| *byte == b'\n') {
                Some(newline) => (&self.remaining[..newline], &self.remaining[newline + 1..]),
                None => (self.remaining, &[]),
            };
        self.remaining = rest;
        Some(line)
    }
}
