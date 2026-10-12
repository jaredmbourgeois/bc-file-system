//! The path module's Rust binding suite, ported from `cpp/tests/path_test.cpp` case for case (21): every free
//! function of `bc_file_system::path` reaches C once and hands its answer back as a value, a view or an enum; a
//! refusal arrives as an `Error` with the operation the core named and the refused bytes as its path; the two
//! enums map totally; and the edge table the C-only test (`core/tests/path_test.c`) holds is run again here,
//! through the face. The rules themselves are proven in C; what these cases hold is that the face carries each
//! answer across unchanged. No case touches the disk.

use std::ffi::OsStr;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

use bc_file_system::path::{
    self, LibraryKind, LibraryName, PathRelation, components, extension, join, last_component,
    lexically_normalize, library_name, parent, relate, relate_loose, resolve_against, stem,
};
use bc_file_system::{AbsolutePath, Error, ErrorKind, Operation, RelativePath};

fn absolute(text: &str) -> AbsolutePath {
    AbsolutePath::from_bytes(text.as_bytes()).expect("an absolute path")
}

fn relative(text: &str) -> RelativePath {
    RelativePath::from_bytes(text.as_bytes()).expect("a relative path")
}

/// The text as the loose `&OsStr` the module's loose functions take.
fn loose(text: &str) -> &OsStr {
    OsStr::new(text)
}

/// What every refusal of the module looks like through the face.
fn assert_refusal(error: &Error, operation: Operation, refused_bytes: &[u8]) {
    assert_eq!(error.kind, ErrorKind::InvalidArgument);
    assert_eq!(error.operation, operation);
    assert_eq!(error.step, None);
    assert_eq!(error.system_code, 0);
    assert_eq!(error.path.as_os_str().as_bytes(), refused_bytes);
    assert!(!error.name_truncated);
}

fn library(kind: LibraryKind, stem: &str) -> LibraryName {
    LibraryName {
        kind,
        stem: stem.into(),
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Each function reaches C and hands its answer back
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn join_of_an_absolute_base_is_absolute() {
    let joined = join(&absolute("/a"), &relative("b/c")).expect("joined");
    let under_root = join(&absolute("/"), &relative("a")).expect("joined");

    assert_eq!(joined, absolute("/a/b/c"));
    assert_eq!(under_root, absolute("/a"));
}

#[test]
fn join_of_a_relative_base_is_relative() {
    let joined = join(&relative("a"), &relative("b")).expect("joined");

    assert_eq!(joined, relative("a/b"));
}

#[test]
fn parent_is_a_prefix_or_absent() {
    assert_eq!(
        parent(&absolute("/a/b")).expect("answered"),
        Some(absolute("/a"))
    );
    assert_eq!(
        parent(&absolute("/a")).expect("answered"),
        Some(absolute("/"))
    );
    assert_eq!(parent(&absolute("/")).expect("answered"), None);
    assert_eq!(
        parent(&relative("a/b/c")).expect("answered"),
        Some(relative("a/b"))
    );
    assert_eq!(parent(&relative("a")).expect("answered"), None);
}

#[test]
fn last_component_of_a_path_and_the_roots_refusal() {
    let of_absolute = absolute("/a/b.txt");
    let of_relative = relative("a");
    let root = absolute("/");

    assert_eq!(last_component(&of_absolute).expect("answered"), "b.txt");
    assert_eq!(last_component(&of_relative).expect("answered"), "a");
    let refused = last_component(&root).expect_err("the root has no component");
    assert_refusal(&refused, Operation::PathLastComponent, b"/");
}

#[test]
fn components_in_order_and_none_for_the_root() {
    let of_absolute = absolute("/a/../bb");
    let of_relative = relative("x/y");
    let root = absolute("/");

    assert_eq!(
        components(&of_absolute).expect("answered"),
        ["a", "..", "bb"]
    );
    assert_eq!(components(&of_relative).expect("answered"), ["x", "y"]);
    assert!(components(&root).expect("answered").is_empty());
}

#[test]
fn stem_and_extension_of_one_component() {
    assert_eq!(
        stem(loose("archive.tar.gz")).expect("answered"),
        "archive.tar"
    );
    assert_eq!(
        extension(loose("archive.tar.gz")).expect("answered"),
        Some(loose("gz"))
    );
    assert_eq!(extension(loose(".bashrc")).expect("answered"), None);
}

#[test]
fn relate_through_each_form() {
    assert_eq!(
        relate(&absolute("/a/b"), &absolute("/a")).expect("answered"),
        PathRelation::Inside
    );
    assert_eq!(
        relate(&relative("a"), &relative("a/b")).expect("answered"),
        PathRelation::Outside
    );
    assert_eq!(
        relate_loose(loose("a/../b"), loose("b")).expect("answered"),
        PathRelation::Same
    );
    assert_eq!(
        relate_loose(loose("/a"), loose("a")).expect("answered"),
        PathRelation::Undecidable
    );
}

#[test]
fn lexically_normalize_answers_the_whole_text() {
    assert_eq!(
        lexically_normalize(loose("/a/./b//../c/")).expect("answered"),
        Path::new("/a/c")
    );
}

#[test]
fn resolve_against_answers_an_absolute_path() {
    let base = absolute("/home/me");

    assert_eq!(
        resolve_against(&base, loose("docs/../notes.txt")).expect("answered"),
        absolute("/home/me/notes.txt")
    );
    assert_eq!(
        resolve_against(&absolute("/a"), loose("../../b")).expect("answered"),
        absolute("/b")
    );
}

#[test]
fn library_name_is_a_kind_and_a_stem_or_none() {
    assert_eq!(
        library_name(loose("libz.so.1")).expect("answered"),
        Some(library(LibraryKind::SharedLibrary, "z"))
    );
    assert_eq!(library_name(loose("notes.txt")).expect("answered"), None);
}

// ---------------------------------------------------------------------------------------------------------------
// The error mapping
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn a_refusal_names_the_operation_and_the_refused_bytes() {
    let refused_stem = stem(loose("a/b")).expect_err("not one component");
    let refused_extension = extension(loose("")).expect_err("empty");
    let refused_relation = relate_loose(loose(""), loose("a")).expect_err("empty");
    let refused_normalization = lexically_normalize(loose("")).expect_err("empty");
    let refused_resolution =
        resolve_against(&absolute("/a"), loose("/b")).expect_err("a leading slash");
    let refused_library = library_name(loose("a/b")).expect_err("not one component");

    assert_refusal(&refused_stem, Operation::PathStem, b"a/b");
    assert_refusal(&refused_extension, Operation::PathExtension, b"");
    assert_refusal(&refused_relation, Operation::PathRelate, b"");
    assert_refusal(
        &refused_normalization,
        Operation::PathLexicallyNormalize,
        b"",
    );
    assert_refusal(&refused_resolution, Operation::PathResolveAgainst, b"/b");
    assert_refusal(&refused_library, Operation::PathLibraryName, b"a/b");
}

#[test]
fn a_refusals_message_reads_like_every_other_errors() {
    let refused = stem(loose("a/b")).expect_err("not one component");

    assert_eq!(refused.to_string(), "path_stem: a/b: invalid_argument");
}

#[test]
fn a_nul_byte_inside_loose_text_is_refused() {
    let text = OsStr::from_bytes(b"a\0b");

    let refused = lexically_normalize(text).expect_err("a NUL inside");

    assert_eq!(refused.kind, ErrorKind::InvalidArgument);
    assert_eq!(refused.path.as_os_str(), text);
}

#[test]
fn every_path_relation_has_its_name_and_its_c_value() {
    let names = ["inside", "same", "outside", "undecidable"];

    for (value, name) in (1..).zip(names) {
        let relation = PathRelation::try_from(value).expect("a relation for every value");
        assert_eq!(relation.name(), name);
        assert_eq!(i32::from(relation), value);
    }
    assert_eq!(PathRelation::ALL.len(), names.len());
    assert!(PathRelation::try_from(0).is_err());
    assert!(PathRelation::try_from(5).is_err());
}

// The header's `none` (1) is `library_name`'s `None`, never a kind, so the enum runs from 2.
#[test]
fn every_library_kind_has_its_name_and_its_c_value() {
    let names = ["shared_library", "static_library", "framework"];

    for (value, name) in (2..).zip(names) {
        let kind = LibraryKind::try_from(value).expect("a kind for every value");
        assert_eq!(kind.name(), name);
        assert_eq!(i32::from(kind), value);
    }
    assert_eq!(LibraryKind::ALL.len(), names.len());
    assert!(LibraryKind::try_from(1).is_err(), "none is no kind");
    assert!(LibraryKind::try_from(0).is_err());
    assert!(LibraryKind::try_from(5).is_err());
}

// ---------------------------------------------------------------------------------------------------------------
// The edge table, through the face
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn lexically_normalize_table() {
    let cases = [
        ("a//b", "a/b"),
        ("a/./b", "a/b"),
        ("a/../b", "b"),
        ("../a", "../a"),
        ("a/../../b", "../b"),
        ("/..", "/"),
        ("//", "/"),
        ("/a/", "/a"),
        (".", "."),
        ("a/..", "."),
        ("./", "."),
        ("/a/b/../c", "/a/c"),
        ("a/b/../../..", ".."),
        ("./a/./", "a"),
        ("/./a", "/a"),
        ("/", "/"),
        ("a", "a"),
        ("/a/../../b", "/b"),
        ("..", ".."),
        ("a/b/c/../..", "a"),
    ];
    for (text, expected) in cases {
        let normalized = lexically_normalize(loose(text)).expect("answered");
        assert_eq!(normalized, Path::new(expected), "{text}");
        let again = lexically_normalize(normalized.as_os_str()).expect("answered");
        assert_eq!(again, normalized, "idempotent: {text}");
    }
}

#[test]
fn relate_table() {
    let cases = [
        ("/a/b", "/a", PathRelation::Inside),
        ("/a", "/a/b", PathRelation::Outside),
        ("/a", "/a", PathRelation::Same),
        ("/a/./b", "/a/b", PathRelation::Same),
        ("/a/b/..", "/a", PathRelation::Same),
        ("/a", "/b", PathRelation::Outside),
        ("/ab", "/a", PathRelation::Outside),
        ("/", "/", PathRelation::Same),
        ("/a", "/", PathRelation::Inside),
        ("/..", "/", PathRelation::Same),
        ("a/b", "a", PathRelation::Inside),
        ("a/..", ".", PathRelation::Same),
        ("a", ".", PathRelation::Inside),
        ("a", "/a", PathRelation::Undecidable),
        ("../a", "a", PathRelation::Undecidable),
        ("a/../../b", "b", PathRelation::Undecidable),
    ];
    for (path, other, expected) in cases {
        let relation = relate_loose(loose(path), loose(other)).expect("answered");
        assert_eq!(relation, expected, "{path} vs {other}");
    }
}

#[test]
fn join_of_validated_parts_validates() {
    let cases = [
        ("/", "a", "/a"),
        ("/", "a/b", "/a/b"),
        ("/a", "b", "/a/b"),
        ("/a", "b/c", "/a/b/c"),
        ("/a/..", "b", "/a/../b"),
    ];
    for (base, relative_text, expected) in cases {
        let joined = join(&absolute(base), &relative(relative_text)).expect("joined");
        assert_eq!(joined, absolute(expected));
        // The parent and last component of the answer are the parts that made it, when the relative part
        // was one component.
        if !relative_text.contains('/') {
            assert_eq!(parent(&joined).expect("answered"), Some(absolute(base)));
            assert_eq!(last_component(&joined).expect("answered"), relative_text);
        }
    }
}

#[test]
fn stem_and_extension_table() {
    let cases = [
        ("a.txt", "a", Some("txt")),
        ("archive.tar.gz", "archive.tar", Some("gz")),
        (".bashrc", ".bashrc", None),
        ("archive.", "archive.", None),
        ("a", "a", None),
        (".a.b", ".a", Some("b")),
        (".", ".", None),
        ("..", "..", None),
    ];
    for (component, expected_stem, expected_extension) in cases {
        assert_eq!(
            stem(loose(component)).expect("answered"),
            expected_stem,
            "{component}"
        );
        assert_eq!(
            extension(loose(component)).expect("answered"),
            expected_extension.map(loose),
            "{component}"
        );
    }
}

#[test]
fn library_name_table() {
    let cases = [
        ("libz.so", Some(library(LibraryKind::SharedLibrary, "z"))),
        (
            "libfoo.so.1.2.3",
            Some(library(LibraryKind::SharedLibrary, "foo")),
        ),
        (
            "libc++.dylib",
            Some(library(LibraryKind::SharedLibrary, "c++")),
        ),
        (
            "libz.1.dylib",
            Some(library(LibraryKind::SharedLibrary, "z.1")),
        ),
        ("libm.a", Some(library(LibraryKind::StaticLibrary, "m"))),
        (
            "Foundation.framework",
            Some(library(LibraryKind::Framework, "Foundation")),
        ),
        (
            "kernel32.dll",
            Some(library(LibraryKind::SharedLibrary, "kernel32")),
        ),
        ("libfoo.so.1.x", None),
        ("lib.so", None),
        ("libz.SO", None),
        ("foo.so", None),
        ("a", None),
    ];
    for (component, expected) in cases {
        assert_eq!(
            library_name(loose(component)).expect("answered"),
            expected,
            "{component}"
        );
    }
}

// NAME_MAX is 255 on every platform the library ships for (Linux and Apple alike), so this edge holds on both.
#[test]
fn a_component_of_255_bytes_is_accepted_and_one_of_256_refused() {
    let longest = "x".repeat(255);
    let too_long = "x".repeat(256);

    let accepted = RelativePath::from_bytes(format!("a/{longest}").as_bytes());
    let refused = RelativePath::from_bytes(format!("a/{too_long}").as_bytes());
    let refused_stem = path::stem(loose(&too_long));

    assert!(accepted.is_ok());
    assert_eq!(
        refused.map_err(|error| error.kind),
        Err(ErrorKind::InvalidArgument)
    );
    assert_eq!(
        refused_stem.map_err(|error| error.kind),
        Err(ErrorKind::InvalidArgument)
    );
}
