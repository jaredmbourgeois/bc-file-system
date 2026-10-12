import BCFileSystem
import CBCFileSystem
import Testing

// The path module's Swift binding suite, the port of the C++ face's `cpp/tests/path_test.cpp` case for case, each
// case named as the C++ one is: every function of `PathText` reaches C once and hands its answer back as a path
// value, bytes or an enum; a refusal arrives as a `FileSystemError` with the operation the core named and the
// refused bytes as its path; the two enums map totally; and the edge tables of the C-only test
// (`core/tests/path_test.c`) run again through the face, with the rows the C table holds beyond the C++ one. The
// rules themselves are proven in C; these cases hold that the face carries each answer across unchanged. Nothing
// here touches the disk, so no case owns a directory.

/// The bytes of `text`, as every function of the module takes and answers them.
private func bytes(_ text: String) -> [UInt8] {
    Array(text.utf8)
}

private func absolute(_ text: String) throws(FileSystemError) -> AbsolutePath {
    try AbsolutePath(validating: text)
}

private func relative(_ text: String) throws(FileSystemError) -> RelativePath {
    try RelativePath(validating: text)
}

/// The error `operation` throws; records an issue and answers nil when it throws nothing.
private func errorThrown<T>(
    sourceLocation: SourceLocation = #_sourceLocation,
    by operation: () throws(FileSystemError) -> T
) -> FileSystemError? {
    do {
        _ = try operation()
        Issue.record("expected an error", sourceLocation: sourceLocation)
        return nil
    } catch {
        return error
    }
}

/// What every refusal of the module looks like through the face.
private func expectRefusal(
    _ error: FileSystemError?,
    operation: Operation,
    refusedText: String,
    sourceLocation: SourceLocation = #_sourceLocation
) {
    #expect(error?.kind == .invalidArgument, sourceLocation: sourceLocation)
    #expect(error?.operation == operation, sourceLocation: sourceLocation)
    #expect(error?.step == nil, sourceLocation: sourceLocation)
    #expect(error?.systemCode == 0, sourceLocation: sourceLocation)
    #expect(error?.path == refusedText, sourceLocation: sourceLocation)
    #expect(error?.nameTruncated == false, sourceLocation: sourceLocation)
}

/// Each function reaches C and hands its answer back.
struct PathBindingTests {
    @Test func joinOfAnAbsoluteBaseIsAbsolute() throws {
        let joined = try PathText.join(absolute("/a"), relative("b/c"))
        let underRoot = try PathText.join(absolute("/"), relative("a"))

        #expect(joined.text == "/a/b/c")
        #expect(underRoot.text == "/a")
    }

    @Test func joinOfARelativeBaseIsRelative() throws {
        let joined = try PathText.join(relative("a"), relative("b"))

        #expect(joined.text == "a/b")
    }

    @Test func parentIsAPrefixOrAbsent() throws {
        let ofFile = try PathText.parent(of: absolute("/a/b"))
        let ofTop = try PathText.parent(of: absolute("/a"))
        let ofRoot = try PathText.parent(of: absolute("/"))
        let ofNested = try PathText.parent(of: relative("a/b/c"))
        let ofOne = try PathText.parent(of: relative("a"))

        #expect(ofFile?.text == "/a")
        #expect(ofTop?.text == "/")
        #expect(ofRoot == nil)
        #expect(ofNested?.text == "a/b")
        #expect(ofOne == nil)
    }

    @Test func lastComponentOfAPathAndTheRootsRefusal() throws {
        let ofAbsolute = try PathText.lastComponent(of: absolute("/a/b.txt"))
        let ofRelative = try PathText.lastComponent(of: relative("a"))
        let root = try absolute("/")
        let ofRoot = errorThrown { () throws(FileSystemError) in try PathText.lastComponent(of: root) }

        #expect(ofAbsolute == bytes("b.txt"))
        #expect(ofRelative == bytes("a"))
        expectRefusal(ofRoot, operation: .pathLastComponent, refusedText: "/")
    }

    @Test func componentsInOrderAndNoneForTheRoot() throws {
        let ofAbsolute = try PathText.components(of: absolute("/a/../bb"))
        let ofRelative = try PathText.components(of: relative("x/y"))
        let ofRoot = try PathText.components(of: absolute("/"))

        #expect(ofAbsolute == [bytes("a"), bytes(".."), bytes("bb")])
        #expect(ofRelative == [bytes("x"), bytes("y")])
        #expect(ofRoot.isEmpty)
    }

    @Test func stemAndExtensionOfOneComponent() throws {
        let stem = try PathText.stem(of: bytes("archive.tar.gz"))
        let fileExtension = try PathText.extension(of: bytes("archive.tar.gz"))
        let none = try PathText.extension(of: bytes(".bashrc"))

        #expect(stem == bytes("archive.tar"))
        #expect(fileExtension == bytes("gz"))
        #expect(none == nil)
    }

    @Test func relateThroughEachOverload() throws {
        let typedAbsolute = try PathText.relate(absolute("/a/b"), to: absolute("/a"))
        let typedRelative = try PathText.relate(relative("a"), to: relative("a/b"))
        let loose = try PathText.relate(bytes("a/../b"), to: bytes("b"))
        let mixed = try PathText.relate(bytes("/a"), to: bytes("a"))

        #expect(typedAbsolute == .inside)
        #expect(typedRelative == .outside)
        #expect(loose == .same)
        #expect(mixed == .undecidable)
    }

    @Test func lexicallyNormalizeAnswersTheWholeText() throws {
        let normalized = try PathText.lexicallyNormalize(bytes("/a/./b//../c/"))

        #expect(normalized == bytes("/a/c"))
    }

    @Test func resolveAgainstAnswersAnAbsolutePath() throws {
        let resolved = try PathText.resolve(bytes("docs/../notes.txt"), against: absolute("/home/me"))
        let clamped = try PathText.resolve(bytes("../../b"), against: absolute("/a"))

        #expect(resolved.text == "/home/me/notes.txt")
        #expect(clamped.text == "/b")
    }

    @Test func libraryNameIsAKindAndAStemOrEmpty() throws {
        let shared = try PathText.libraryName(of: bytes("libz.so.1"))
        let plain = try PathText.libraryName(of: bytes("notes.txt"))

        #expect(shared == LibraryName(kind: .sharedLibrary, stem: bytes("z")))
        #expect(plain == nil)
    }
}

/// The error mapping.
struct PathErrorTests {
    @Test func aRefusalNamesTheOperationAndTheRefusedBytes() throws {
        let base = try absolute("/a")

        let stem = errorThrown { () throws(FileSystemError) in try PathText.stem(of: bytes("a/b")) }
        let fileExtension = errorThrown { () throws(FileSystemError) in try PathText.extension(of: []) }
        let relation = errorThrown { () throws(FileSystemError) in try PathText.relate([], to: bytes("a")) }
        let normalized = errorThrown { () throws(FileSystemError) in try PathText.lexicallyNormalize([]) }
        let resolved = errorThrown { () throws(FileSystemError) in
            try PathText.resolve(bytes("/b"), against: base)
        }
        let library = errorThrown { () throws(FileSystemError) in try PathText.libraryName(of: bytes("a/b")) }

        expectRefusal(stem, operation: .pathStem, refusedText: "a/b")
        expectRefusal(fileExtension, operation: .pathExtension, refusedText: "")
        expectRefusal(relation, operation: .pathRelate, refusedText: "")
        expectRefusal(normalized, operation: .pathLexicallyNormalize, refusedText: "")
        expectRefusal(resolved, operation: .pathResolveAgainst, refusedText: "/b")
        expectRefusal(library, operation: .pathLibraryName, refusedText: "a/b")
    }

    @Test func aRefusalsMessageReadsLikeEveryOtherErrors() {
        let stem = errorThrown { () throws(FileSystemError) in try PathText.stem(of: bytes("a/b")) }

        #expect(stem?.message == "pathStem: a/b: invalidArgument")
    }

    @Test func aNulByteInsideLooseTextIsRefused() {
        let text: [UInt8] = [0x61, 0, 0x62]

        let normalized = errorThrown { () throws(FileSystemError) in try PathText.lexicallyNormalize(text) }

        #expect(normalized?.kind == .invalidArgument)
        #expect(normalized?.path == String(decoding: text, as: UTF8.self))
    }

    @Test func everyPathRelationHasItsNameAndItsCValue() {
        let pairs: [(bc_file_system_path_relation, PathRelation, String)] = [
            (bc_file_system_path_relation_inside, .inside, "inside"),
            (bc_file_system_path_relation_same, .same, "same"),
            (bc_file_system_path_relation_outside, .outside, "outside"),
            (bc_file_system_path_relation_undecidable, .undecidable, "undecidable"),
        ]
        for (core, relation, name) in pairs {
            #expect(PathRelation(rawValue: Int32(core.rawValue)) == relation)
            #expect("\(relation)" == name)
        }
        #expect(PathRelation.allCases.count == pairs.count)
        #expect(PathRelation.allCases.count == Int(bc_file_system_path_relation_undecidable.rawValue))
        #expect(PathRelation(rawValue: 0) == nil)
        #expect(PathRelation(rawValue: 5) == nil)
    }

    /// The header's `none` has no case: the face answers a component that spells no library as nil, so its value
    /// maps to no kind, and the three kinds a `LibraryName` can hold map one to one.
    @Test func everyLibraryKindHasItsNameAndItsCValue() {
        let pairs: [(bc_file_system_library_kind, LibraryKind, String)] = [
            (bc_file_system_library_kind_shared, .sharedLibrary, "sharedLibrary"),
            (bc_file_system_library_kind_static, .staticLibrary, "staticLibrary"),
            (bc_file_system_library_kind_framework, .framework, "framework"),
        ]
        for (core, kind, name) in pairs {
            #expect(LibraryKind(rawValue: Int32(core.rawValue)) == kind)
            #expect("\(kind)" == name)
        }
        #expect(LibraryKind.allCases.count == pairs.count)
        #expect(LibraryKind(rawValue: Int32(bc_file_system_library_kind_none.rawValue)) == nil)
        #expect(LibraryKind(rawValue: 0) == nil)
        #expect(LibraryKind(rawValue: 5) == nil)
    }
}

/// The edge tables, through the face.
struct PathEdgeTests {
    /// Normalisation keeps a leading `..` of a relative path, drops one at the root, and answers `.` when nothing is
    /// left; every answer normalises to itself.
    @Test func lexicallyNormalizeTable() throws {
        let cases: [(text: String, expected: String)] = [
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
            ("/a/b/c/../../d", "/a/d"),
            ("...", "..."),
            ("a/.../b", "a/.../b"),
        ]
        for (text, expected) in cases {
            let normalized = try PathText.lexicallyNormalize(bytes(text))
            #expect(normalized == bytes(expected), "\(text)")
            #expect(try PathText.lexicallyNormalize(normalized) == normalized, "idempotent: \(text)")
        }
    }

    /// `relate`'s readings, from either side: `inside` reversed is `outside`; `same` and `undecidable` read the same
    /// from both sides. `outside` reversed is either: `/a` is outside `/a/b`, which is inside `/a`, while two
    /// siblings are outside each other.
    @Test func relateTable() throws {
        let cases: [(path: String, other: String, relation: PathRelation)] = [
            ("/a/b", "/a", .inside),
            ("/a", "/a/b", .outside),
            ("/a", "/a", .same),
            ("/a/./b", "/a/b", .same),
            ("/a/b/..", "/a", .same),
            ("/a//b/", "/a/b", .same),
            ("/a/b/c", "/a", .inside),
            ("/a", "/b", .outside),
            ("/ab", "/a", .outside),
            ("/", "/", .same),
            ("/a", "/", .inside),
            ("/..", "/", .same),
            ("/a/../..", "/", .same),
            ("a/b", "a", .inside),
            ("a", "a/b", .outside),
            ("a/..", ".", .same),
            ("a", ".", .inside),
            (".", "a", .outside),
            ("a", "/a", .undecidable),
            ("/a", "a", .undecidable),
            ("../a", "a", .undecidable),
            ("a", "../a", .undecidable),
            ("a/../../b", "b", .undecidable),
        ]
        for (path, other, relation) in cases {
            let forward = try PathText.relate(bytes(path), to: bytes(other))
            let reverse = try PathText.relate(bytes(other), to: bytes(path))
            #expect(forward == relation, "\(path) vs \(other)")
            switch relation {
            case .inside:
                #expect(reverse == .outside, "inside reversed is outside: \(path) vs \(other)")
            case .same, .undecidable:
                #expect(reverse == relation, "symmetric: \(path) vs \(other)")
            case .outside:
                break
            }
        }
    }

    /// A join of two valid parts is valid; when the relative part is one component, the answer's parent and last
    /// component are the parts that made it.
    @Test func joinOfValidatedPartsValidates() throws {
        let cases: [(base: String, relative: String, expected: String)] = [
            ("/", "a", "/a"),
            ("/", "a/b", "/a/b"),
            ("/a", "b", "/a/b"),
            ("/a", "b/c", "/a/b/c"),
            ("/a/..", "b", "/a/../b"),
        ]
        for (base, relativeText, expected) in cases {
            let joined = try PathText.join(absolute(base), relative(relativeText))
            #expect(joined.text == expected, "\(base) + \(relativeText)")
            if !relativeText.contains("/") {
                #expect(try PathText.parent(of: joined)?.text == base, "\(expected)")
                #expect(try PathText.lastComponent(of: joined) == bytes(relativeText), "\(expected)")
            }
        }
    }

    /// The extension follows the last dot when that dot is neither the first byte nor the last; the stem is the
    /// rest, so a stem, a dot and an extension make the whole component.
    @Test func stemAndExtensionTable() throws {
        let cases: [(component: String, stem: String, fileExtension: String?)] = [
            ("a.txt", "a", "txt"),
            ("archive.tar.gz", "archive.tar", "gz"),
            (".bashrc", ".bashrc", nil),
            ("archive.", "archive.", nil),
            ("a", "a", nil),
            (".a.b", ".a", "b"),
            (".", ".", nil),
            ("..", "..", nil),
            ("...", "...", nil),
            ("a..b", "a.", "b"),
        ]
        for (component, stem, fileExtension) in cases {
            #expect(try PathText.stem(of: bytes(component)) == bytes(stem), "\(component)")
            let answered = try PathText.extension(of: bytes(component))
            if let fileExtension {
                #expect(answered == bytes(fileExtension), "\(component)")
            } else {
                #expect(answered == nil, "\(component)")
            }
        }
    }

    @Test func libraryNameTable() throws {
        let cases: [(component: String, expected: LibraryName?)] = [
            ("libz.so", LibraryName(kind: .sharedLibrary, stem: bytes("z"))),
            ("libz.so.1", LibraryName(kind: .sharedLibrary, stem: bytes("z"))),
            ("libfoo.so.1.2.3", LibraryName(kind: .sharedLibrary, stem: bytes("foo"))),
            ("libfoo.so.bar.so", LibraryName(kind: .sharedLibrary, stem: bytes("foo.so.bar"))),
            ("libc++.dylib", LibraryName(kind: .sharedLibrary, stem: bytes("c++"))),
            ("libz.1.dylib", LibraryName(kind: .sharedLibrary, stem: bytes("z.1"))),
            ("libm.a", LibraryName(kind: .staticLibrary, stem: bytes("m"))),
            ("Foundation.framework", LibraryName(kind: .framework, stem: bytes("Foundation"))),
            ("kernel32.dll", LibraryName(kind: .sharedLibrary, stem: bytes("kernel32"))),
            ("libfoo.dll", LibraryName(kind: .sharedLibrary, stem: bytes("libfoo"))),
            ("libfoo.so.1.x", nil),
            ("libfoo.so.", nil),
            ("libfoo.so..1", nil),
            ("lib.so", nil),
            ("lib.a", nil),
            ("libz.SO", nil),
            ("foo.so", nil),
            (".dll", nil),
            (".framework", nil),
            ("libfoo", nil),
            ("a", nil),
            ("notes.txt", nil),
        ]
        for (component, expected) in cases {
            #expect(try PathText.libraryName(of: bytes(component)) == expected, "\(component)")
        }
    }

    /// 255 bytes is NAME_MAX on every platform the library ships for, Linux and Apple alike.
    @Test func aComponentOf255BytesIsAcceptedAndOneOf256Refused() throws {
        let longest = String(repeating: "x", count: 255)
        let tooLong = String(repeating: "x", count: 256)

        let accepted = try RelativePath(validating: "a/" + longest)
        let refused = errorThrown { () throws(FileSystemError) in try RelativePath(validating: "a/" + tooLong) }
        let stem = errorThrown { () throws(FileSystemError) in try PathText.stem(of: bytes(tooLong)) }

        #expect(accepted.text == "a/" + longest)
        #expect(refused?.kind == .invalidArgument)
        #expect(stem?.kind == .invalidArgument)
    }

    /// The face never hands out the module's cursor (`components(of:)` runs the iteration to its end), so the
    /// module's refusal of a cursor it never produced, here one inside a component, is held at the C level.
    @Test func nextComponentRefusesACursorItNeverProduced() {
        let path = bytes("ab")
        var cursor = 1
        var offset = 0
        var componentLength = 0
        var present: Int32 = 7
        var error = bc_file_system_error()

        let outcome = path.withUnsafeBufferPointer { buffer in
            bc_file_system_path_next_component(
                buffer.baseAddress, buffer.count, &cursor, &offset, &componentLength, &present, &error)
        }

        #expect(outcome == Int32(bc_file_system_error_kind_invalid_argument.rawValue))
        #expect(present == 7)
        #expect(error.operation == Int32(bc_file_system_operation_path_next_component.rawValue))
    }
}
