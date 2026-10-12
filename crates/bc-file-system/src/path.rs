//! Path text: the two validated path values and the ten functions of the path module, mirroring
//! `cpp/include/bc_file_system/path.hpp` one to one.
//!
//! Every rule lives once, in C (`core/src/path.c`, the header's "Path text"): these functions hold none, and
//! nothing here consults the disk, the working directory or the environment. A path is bytes; a value is made
//! from a `&Path` (an `&OsStr` is `Path::new(text)`) or from `&[u8]`, never from `&str` alone, since a path need
//! not be UTF-8. A typed answer (an [`AbsolutePath`], a [`RelativePath`]) is validated in C once more on the way
//! out, so the face never holds bytes the core did not accept. A refusal is an [`Error`] with the operation the
//! core named, no step, no system code, and as its path the bytes the core refused: the loose argument's where a
//! function takes one, the first path's otherwise.
//!
//! The functions that take a path of either family are generic over [`ValidatedPath`], which only the two values
//! implement: the answer is a path of the argument's own family, as the C++ overloads answer.

use std::ffi::{OsStr, OsString};
use std::ops::Range;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};

use crate::error::{Error, Operation, UnknownValue, path_of_bytes, variant_of};
use crate::ffi;
use crate::live;

/// Absolute path text: a leading slash, `/` alone the root, otherwise one or more components with no empty, `.`
/// or over-255-byte component; `..` is allowed and reported, never resolved here. One spelling per path, so
/// equality is a byte compare.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct AbsolutePath {
    path: PathBuf,
}

/// Path text relative to a directory: one or more components, no leading slash, no NUL, no empty, `.` or `..`
/// component, none over 255 bytes. One spelling per path, so equality is a byte compare.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct RelativePath {
    path: PathBuf,
}

impl AbsolutePath {
    /// `path` as an absolute path, validated by the path module (operation `absolute_path_from`).
    ///
    /// # Errors
    ///
    /// `InvalidArgument`, with `path` as the error's path, for text that breaks the rules above.
    pub fn from_path(path: &Path) -> Result<Self, Error> {
        Self::from_bytes(path.as_os_str().as_bytes())
    }

    /// `bytes` as an absolute path, validated by the path module (operation `absolute_path_from`).
    ///
    /// # Errors
    ///
    /// `InvalidArgument`, with the bytes as the error's path, for text that breaks the rules above.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, Error> {
        live::absolute_path_validate(bytes)?;
        Ok(Self {
            path: PathBuf::from(OsString::from_vec(bytes.to_vec())),
        })
    }

    /// The path, for the standard library's own calls and for display.
    #[must_use]
    pub fn as_path(&self) -> &Path {
        &self.path
    }

    /// The path's bytes, exactly as validated.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        self.path.as_os_str().as_bytes()
    }
}

impl RelativePath {
    /// `path` as a relative path, validated by the path module (operation `relative_path_from`).
    ///
    /// # Errors
    ///
    /// `InvalidArgument`, with `path` as the error's path, for text that breaks the rules above.
    pub fn from_path(path: &Path) -> Result<Self, Error> {
        Self::from_bytes(path.as_os_str().as_bytes())
    }

    /// `bytes` as a relative path, validated by the path module (operation `relative_path_from`).
    ///
    /// # Errors
    ///
    /// `InvalidArgument`, with the bytes as the error's path, for text that breaks the rules above.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, Error> {
        live::relative_path_validate(bytes)?;
        Ok(Self {
            path: PathBuf::from(OsString::from_vec(bytes.to_vec())),
        })
    }

    /// The path, for the standard library's own calls and for display.
    #[must_use]
    pub fn as_path(&self) -> &Path {
        &self.path
    }

    /// The path's bytes, exactly as validated.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        self.path.as_os_str().as_bytes()
    }
}

/// The private half of [`ValidatedPath`]: in a module no other crate can name, so no other crate can implement
/// the trait, and the functions below may build an answer of the argument's family.
mod sealed {
    use crate::error::Error;

    /// Public so it may bound the public trait; in a private module, so no other crate can name it.
    pub trait Sealed: Sized {
        /// The path's bytes.
        fn bytes(&self) -> &[u8];
        /// Bytes the core answered, validated as a path of this family.
        fn validated(bytes: &[u8]) -> Result<Self, Error>;
    }

    impl Sealed for super::AbsolutePath {
        fn bytes(&self) -> &[u8] {
            self.as_bytes()
        }

        fn validated(bytes: &[u8]) -> Result<Self, Error> {
            Self::from_bytes(bytes)
        }
    }

    impl Sealed for super::RelativePath {
        fn bytes(&self) -> &[u8] {
            self.as_bytes()
        }

        fn validated(bytes: &[u8]) -> Result<Self, Error> {
            Self::from_bytes(bytes)
        }
    }
}

/// A path value of either family, [`AbsolutePath`] or [`RelativePath`]: the bound of the functions below that
/// take either and answer the same family. Sealed: no other type implements it.
pub trait ValidatedPath: sealed::Sealed {}

impl ValidatedPath for AbsolutePath {}
impl ValidatedPath for RelativePath {}

/// Where one path stands with respect to another, lexically ([`relate`]). Advisory: a lexical answer knows
/// nothing of symlinks or the disk; a location beneath a directory is what guards an access.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PathRelation {
    /// The path names something strictly beneath the other.
    Inside = 1,
    /// The two name the same thing.
    Same = 2,
    /// Neither beneath the other nor the other itself; the other being beneath the path is `Outside` too.
    Outside = 3,
    /// One path is absolute and the other relative, or one of them climbs out of its own start with `..`.
    Undecidable = 4,
}

impl PathRelation {
    /// Every relation, in the header's order.
    pub const ALL: [Self; 4] = [Self::Inside, Self::Same, Self::Outside, Self::Undecidable];

    /// The relation's name as the header spells it.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Inside => "inside",
            Self::Same => "same",
            Self::Outside => "outside",
            Self::Undecidable => "undecidable",
        }
    }
}

impl From<PathRelation> for i32 {
    fn from(relation: PathRelation) -> Self {
        relation as Self
    }
}

impl TryFrom<i32> for PathRelation {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(&Self::ALL, value, "bc_file_system_path_relation")
    }
}

/// What a component spells when it spells a library file. The header's `none` is not a kind here: a component
/// that spells no library is [`library_name`]'s `None`, so a [`LibraryName`] always carries a real kind.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum LibraryKind {
    /// `libX.so`, `libX.so.1.2`, `libX.dylib`, `X.dll`.
    SharedLibrary = 2,
    /// `libX.a`.
    StaticLibrary = 3,
    /// `X.framework`.
    Framework = 4,
}

impl LibraryKind {
    /// Every kind a library name carries, in the header's order (after `none`, which is no kind).
    pub const ALL: [Self; 3] = [Self::SharedLibrary, Self::StaticLibrary, Self::Framework];

    /// The kind's name, for messages and tests.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::SharedLibrary => "shared_library",
            Self::StaticLibrary => "static_library",
            Self::Framework => "framework",
        }
    }
}

impl From<LibraryKind> for i32 {
    fn from(kind: LibraryKind) -> Self {
        kind as Self
    }
}

impl TryFrom<i32> for LibraryKind {
    type Error = UnknownValue;

    /// The kind a value names; the header's `none` (1) is refused like any value outside the enum, since it is
    /// no kind (see [`library_name`]).
    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(
            &Self::ALL,
            value,
            "bc_file_system_library_kind other than none",
        )
    }
}

/// A component recognised as a library file: its kind, and the library's own name (`z` in `libz.so.1`).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct LibraryName {
    /// How the component spells a library.
    pub kind: LibraryKind,
    /// The library's own name inside the component.
    pub stem: OsString,
}

/// The bytes the core located inside `text`, or an error should the core ever answer a range outside it: the
/// module answers ranges inside the text it was handed (the C-only test proves it), and a foreign range is still
/// refused rather than trusted, as an unknown enum value is.
fn located(text: &[u8], range: Range<usize>, called: Operation) -> Result<&[u8], Error> {
    let end = range.end;
    text.get(range).ok_or_else(|| {
        let unknown = UnknownValue {
            enumeration: "a range inside the text",
            value: i32::try_from(end).unwrap_or(i32::MAX),
        };
        Error::answer_outside_enum(called, unknown, path_of_bytes(text))
    })
}

/// `base`, a slash and `relative`; the root already ends in its slash, so `/` and `a` join as `/a`. The answer
/// is a path of `base`'s family.
///
/// # Errors
///
/// `TooLarge` when the lengths do not fit `usize`; `InvalidArgument` should the answer not validate.
pub fn join<P: ValidatedPath>(base: &P, relative: &RelativePath) -> Result<P, Error> {
    let joined = live::path_join(base.bytes(), relative.as_bytes())?;
    P::validated(&joined)
}

/// The parent, a prefix of the path: `/` for `/a`, `a/b` for `a/b/c`. The root and a one-component relative
/// path have none.
///
/// # Errors
///
/// `InvalidArgument` should the prefix not validate, which the module's rules make impossible.
pub fn parent<P: ValidatedPath>(path: &P) -> Result<Option<P>, Error> {
    let bytes = path.bytes();
    let Some(parent_length) = live::path_parent(bytes)? else {
        return Ok(None);
    };
    let prefix = located(bytes, 0..parent_length, Operation::PathParent)?;
    P::validated(prefix).map(Some)
}

/// The last component's bytes, a view into the path.
///
/// # Errors
///
/// `InvalidArgument` for the root, which has no component.
pub fn last_component<P: ValidatedPath>(path: &P) -> Result<&OsStr, Error> {
    let bytes = path.bytes();
    let range = live::path_last_component(bytes)?;
    located(bytes, range, Operation::PathLastComponent).map(OsStr::from_bytes)
}

/// Every component's bytes, in order, each a view into the path; none for the root. The core's cursor
/// (`bc_file_system_path_next_component`) is driven here, from 0 until the core answers that nothing is left.
///
/// # Errors
///
/// `InvalidArgument` should the core refuse its own cursor, which it never does for a validated path.
pub fn components<P: ValidatedPath>(path: &P) -> Result<Vec<&OsStr>, Error> {
    let bytes = path.bytes();
    let mut found = Vec::new();
    let mut cursor = 0;
    while let Some(range) = live::path_next_component(bytes, &mut cursor)? {
        found.push(OsStr::from_bytes(located(
            bytes,
            range,
            Operation::PathNextComponent,
        )?));
    }
    Ok(found)
}

/// The bytes of one component before its extension, or all of them when it has none: `archive.tar` of
/// `archive.tar.gz`, `.bashrc` of `.bashrc`.
///
/// # Errors
///
/// `InvalidArgument` for text that is not one component (empty, a slash or a NUL inside, over 255 bytes).
pub fn stem(component: &OsStr) -> Result<&OsStr, Error> {
    let bytes = component.as_bytes();
    let stem_length = live::path_stem(bytes)?;
    located(bytes, 0..stem_length, Operation::PathStem).map(OsStr::from_bytes)
}

/// The bytes after a component's last dot, when that dot is neither its first byte (`.bashrc` has none) nor its
/// last (`archive.` has none); the dot itself is excluded.
///
/// # Errors
///
/// `InvalidArgument` for text that is not one component.
pub fn extension(component: &OsStr) -> Result<Option<&OsStr>, Error> {
    let bytes = component.as_bytes();
    let Some(range) = live::path_extension(bytes)? else {
        return Ok(None);
    };
    located(bytes, range, Operation::PathExtension)
        .map(|extension| Some(OsStr::from_bytes(extension)))
}

/// Where `path` stands with respect to `other`, both of one family, each normalised first.
///
/// # Errors
///
/// None for two validated paths; the signature is the module's one convention.
pub fn relate<P: ValidatedPath>(path: &P, other: &P) -> Result<PathRelation, Error> {
    relate_loose(
        OsStr::from_bytes(path.bytes()),
        OsStr::from_bytes(other.bytes()),
    )
}

/// Where loose text `path` stands with respect to loose text `other`, each normalised as
/// [`lexically_normalize`] would and then compared component by component. Two kinds, or a relative path that
/// still begins with `..` once normalised, are `Undecidable`. The C++ face's third `relate` overload.
///
/// # Errors
///
/// `InvalidArgument` for text the module refuses (empty, a NUL inside, a component over 255 bytes), naming
/// `path`'s bytes.
pub fn relate_loose(path: &OsStr, other: &OsStr) -> Result<PathRelation, Error> {
    let bytes = path.as_bytes();
    let relation = live::path_relate(bytes, other.as_bytes())?;
    PathRelation::try_from(relation).map_err(|unknown| {
        Error::answer_outside_enum(Operation::PathRelate, unknown, path_of_bytes(bytes))
    })
}

/// Loose text reduced to one spelling: doubled slashes collapse, `.` components go, `a/..` folds away, a
/// trailing slash goes, `/..` is `/`, a leading `..` of a relative path stays, and a relative path with nothing
/// left is `.`. Idempotent; the disk is never consulted.
///
/// # Errors
///
/// `InvalidArgument` for text the module refuses.
pub fn lexically_normalize(text: &OsStr) -> Result<PathBuf, Error> {
    let bytes = text.as_bytes();
    let normalized = live::path_lexically_normalize(bytes)?;
    Ok(PathBuf::from(OsString::from_vec(normalized)))
}

/// `relative`, loose text with no leading slash, joined to `base` and normalised: how a composition root turns
/// a command-line argument into an absolute path against the working directory it was handed.
///
/// # Errors
///
/// `InvalidArgument` for text the module refuses (a leading slash among them), naming `relative`'s bytes;
/// `TooLarge` when the lengths do not fit `usize`.
pub fn resolve_against(base: &AbsolutePath, relative: &OsStr) -> Result<AbsolutePath, Error> {
    let resolved = live::path_resolve_against(base.as_bytes(), relative.as_bytes())?;
    AbsolutePath::from_bytes(&resolved)
}

/// The library a component names, when it spells one the way a platform does (`libX.so`, `libX.so.1.2`,
/// `libX.dylib`, `libX.a`, `X.framework`, `X.dll`, byte for byte), or `None`.
///
/// # Errors
///
/// `InvalidArgument` for text that is not one component.
pub fn library_name(component: &OsStr) -> Result<Option<LibraryName>, Error> {
    let bytes = component.as_bytes();
    let (kind, stem_range) = live::path_library_name(bytes)?;
    if kind == ffi::LIBRARY_KIND_NONE {
        return Ok(None);
    }
    let kind = LibraryKind::try_from(kind).map_err(|unknown| {
        Error::answer_outside_enum(Operation::PathLibraryName, unknown, path_of_bytes(bytes))
    })?;
    let stem = located(bytes, stem_range, Operation::PathLibraryName)?;
    Ok(Some(LibraryName {
        kind,
        stem: OsString::from_vec(stem.to_vec()),
    }))
}
