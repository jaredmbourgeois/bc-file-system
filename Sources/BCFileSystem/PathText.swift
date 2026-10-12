import CBCFileSystem

// The path module's Swift face: the two path values and the ten functions over them. Every rule about path text
// lives once, in C (the header's "Path text"): a path value is validated by one call into the module when it is
// made, and each function is one call into the module plus the conversion of its answer. Nothing here consults
// the disk, the working directory or the environment, and nothing here restates a rule.

// MARK: - Path values

/// Path text relative to a directory, as bytes, one spelling per path so equality is a byte compare.
///
/// Validated once at construction by the path module: at least one component, no leading slash, no NUL byte, no
/// empty, `.` or `..` component, no component over 255 bytes. Nothing on disk is consulted. A refusal is
/// `invalidArgument` from `relativePathFrom`, with the refused bytes as its path.
public struct RelativePath: Equatable, Sendable {
    public let bytes: [UInt8]

    public init(validating text: String) throws(FileSystemError) {
        try self.init(validatingBytes: Array(text.utf8))
    }

    public init(validatingBytes bytes: [UInt8]) throws(FileSystemError) {
        try callPathModule(.relativePathFrom, refusedText: bytes) { coreError in
            bytes.withUnsafeBufferPointer { buffer in
                bc_file_system_relative_path_validate(buffer.baseAddress, buffer.count, &coreError)
            }
        }
        self.bytes = bytes
    }

    /// The bytes as text, for display.
    public var text: String {
        String(decoding: bytes, as: UTF8.self)
    }
}

/// Absolute path text, as bytes, one spelling per path like a relative path's.
///
/// Validated once at construction by the path module: `/` alone is the root; anything else is a slash followed by
/// components under the relative path's rules, except that `..` is allowed: it is kept and reported, never
/// resolved here. So `/a/`, `//`, `/.` and `/a/./b` are refused. A refusal is `invalidArgument` from
/// `absolutePathFrom`, with the refused bytes as its path.
public struct AbsolutePath: Equatable, Sendable {
    public let bytes: [UInt8]

    public init(validating text: String) throws(FileSystemError) {
        try self.init(validatingBytes: Array(text.utf8))
    }

    public init(validatingBytes bytes: [UInt8]) throws(FileSystemError) {
        try callPathModule(.absolutePathFrom, refusedText: bytes) { coreError in
            bytes.withUnsafeBufferPointer { buffer in
                bc_file_system_absolute_path_validate(buffer.baseAddress, buffer.count, &coreError)
            }
        }
        self.bytes = bytes
    }

    /// The bytes as text, for display.
    public var text: String {
        String(decoding: bytes, as: UTF8.self)
    }
}

// MARK: - The answers' types

/// Where one path stands with respect to another, lexically (`PathText.relate`). Advisory: a lexical answer knows
/// nothing of symlinks or the disk; a `beneath` location is what guards an access. The raw values are the header's.
public enum PathRelation: Int32, CaseIterable, Sendable {
    /// The path names something strictly beneath the other.
    case inside = 1
    case same = 2
    /// Neither beneath the other nor the other itself; the other being beneath the path is `outside` too.
    case outside = 3
    /// One path is absolute and the other relative, or one of them still begins with `..` once normalised, so no
    /// lexical rule knows where it climbs to.
    case undecidable = 4
}

/// What kind of library file a component spells (`PathText.libraryName`). The raw values are the header's. The
/// header's value 1, "none", has no case here: a component that spells no library is answered as nil, so a
/// `LibraryName` can never hold a kind that means "not a library".
public enum LibraryKind: Int32, CaseIterable, Sendable {
    /// `libX.so`, `libX.so.1.2`, `libX.dylib`, `X.dll`.
    case sharedLibrary = 2
    /// `libX.a`.
    case staticLibrary = 3
    /// `X.framework`.
    case framework = 4
}

/// A component recognised as a library file: its kind and the library's own name, `z` in `libz.so.1`.
public struct LibraryName: Equatable, Sendable {
    public let kind: LibraryKind
    public let stem: [UInt8]

    public init(kind: LibraryKind, stem: [UInt8]) {
        self.kind = kind
        self.stem = stem
    }
}

// MARK: - The ten functions

/// The path module's ten functions, as static functions on one namespace: `PathText.join(base, relative)`. An
/// enum with no cases, so nothing can create one; it only groups the functions. One shape for all ten, rather than
/// methods on the path values, because four of them (`stem`, `extension`, `libraryName`, `lexicallyNormalize`)
/// and one form of `relate` take bytes that are no path value (a single component, or text still to be
/// normalised), so a method shape would split the ten across two shapes. The set mirrors the C++ face's free functions in
/// `<bc_file_system/path.hpp>` one for one, with an overload for each path value where the C++ face has one.
///
/// A refusal is a `FileSystemError` like any other: `invalidArgument`, the operation the core named, no step, no
/// system code, and as its path the bytes the core refused: the loose argument's where a function takes one, the
/// first path's otherwise. A path value was validated when it was made, so only the loose arguments (`[UInt8]`: a
/// component, or text the module normalises) are ever refused. A typed answer is made through the path value's
/// own constructor, which validates it in C once more, so the face never holds bytes the module did not accept.
public enum PathText {
    /// `base`, a slash and `relative`; the root already ends in its slash, so `/` and `a` join as `/a`.
    public static func join(_ base: AbsolutePath, _ relative: RelativePath) throws(FileSystemError) -> AbsolutePath {
        try AbsolutePath(validatingBytes: joinedText(base.bytes, relative.bytes))
    }

    /// `base`, a slash and `relative`, a relative path again.
    public static func join(_ base: RelativePath, _ relative: RelativePath) throws(FileSystemError) -> RelativePath {
        try RelativePath(validatingBytes: joinedText(base.bytes, relative.bytes))
    }

    /// The parent, a prefix of the path: `/` for `/a`, `/a` for `/a/b`. The root has none.
    public static func parent(of path: AbsolutePath) throws(FileSystemError) -> AbsolutePath? {
        guard let prefix = try parentText(of: path.bytes) else {
            return nil
        }
        return try AbsolutePath(validatingBytes: prefix)
    }

    /// The parent, a prefix of the path: `a/b` for `a/b/c`. A path of one component has none.
    public static func parent(of path: RelativePath) throws(FileSystemError) -> RelativePath? {
        guard let prefix = try parentText(of: path.bytes) else {
            return nil
        }
        return try RelativePath(validatingBytes: prefix)
    }

    /// The last component's bytes. The root has no component and is refused with `invalidArgument`.
    public static func lastComponent(of path: AbsolutePath) throws(FileSystemError) -> [UInt8] {
        try lastComponentText(of: path.bytes)
    }

    /// The last component's bytes.
    public static func lastComponent(of path: RelativePath) throws(FileSystemError) -> [UInt8] {
        try lastComponentText(of: path.bytes)
    }

    /// Every component's bytes, in order; none for the root. A `..` of an absolute path is a component like any
    /// other.
    public static func components(of path: AbsolutePath) throws(FileSystemError) -> [[UInt8]] {
        try componentTexts(of: path.bytes)
    }

    /// Every component's bytes, in order.
    public static func components(of path: RelativePath) throws(FileSystemError) -> [[UInt8]] {
        try componentTexts(of: path.bytes)
    }

    /// The bytes of one component before its extension, or all of them when it has none: `archive.tar` for
    /// `archive.tar.gz`, `.bashrc` for `.bashrc`, `archive.` for `archive.`.
    public static func stem(of component: [UInt8]) throws(FileSystemError) -> [UInt8] {
        var stemLength = 0
        try callPathModule(.pathStem, refusedText: component) { coreError in
            component.withUnsafeBufferPointer { bytes in
                bc_file_system_path_stem(bytes.baseAddress, bytes.count, &stemLength, &coreError)
            }
        }
        return try located(in: component, offset: 0, length: stemLength, called: .pathStem)
    }

    /// The bytes after a component's last dot, when that dot is neither its first byte (`.bashrc` has none) nor
    /// its last (`archive.` has none); the dot itself is excluded. `extension` is a Swift keyword, so the
    /// declaration quotes it in backticks; a call spells it plainly after the dot: `PathText.extension(of:)`.
    public static func `extension`(of component: [UInt8]) throws(FileSystemError) -> [UInt8]? {
        var offset = 0
        var extensionLength = 0
        var present: Int32 = 0
        try callPathModule(.pathExtension, refusedText: component) { coreError in
            component.withUnsafeBufferPointer { bytes in
                bc_file_system_path_extension(
                    bytes.baseAddress, bytes.count, &offset, &extensionLength, &present, &coreError)
            }
        }
        if present == 0 {
            return nil
        }
        return try located(in: component, offset: offset, length: extensionLength, called: .pathExtension)
    }

    /// Where `path` stands with respect to `other`, both absolute.
    public static func relate(_ path: AbsolutePath, to other: AbsolutePath) throws(FileSystemError) -> PathRelation {
        try relate(path.bytes, to: other.bytes)
    }

    /// Where `path` stands with respect to `other`, both relative.
    public static func relate(_ path: RelativePath, to other: RelativePath) throws(FileSystemError) -> PathRelation {
        try relate(path.bytes, to: other.bytes)
    }

    /// Where `path` stands with respect to `other`: any text the module normalises, each normalised first and then
    /// compared component by component. One absolute and one relative, or a path that still begins with `..`, is
    /// `undecidable`.
    public static func relate(_ path: [UInt8], to other: [UInt8]) throws(FileSystemError) -> PathRelation {
        var relation: Int32 = 0
        try callPathModule(.pathRelate, refusedText: path) { coreError in
            path.withUnsafeBufferPointer { pathBytes in
                other.withUnsafeBufferPointer { otherBytes in
                    bc_file_system_path_relate(
                        pathBytes.baseAddress, pathBytes.count, otherBytes.baseAddress, otherBytes.count, &relation,
                        &coreError)
                }
            }
        }
        guard let answer = PathRelation(rawValue: relation) else {
            throw FileSystemError.answerOutsideEnum(called: .pathRelate, locationText: path)
        }
        return answer
    }

    /// `text` reduced to one spelling: doubled slashes collapse, `.` components go, `a/..` folds away, a trailing
    /// slash goes, `/..` is `/`, a leading `..` of a relative path stays, and a relative path with nothing left is
    /// `.`. Idempotent; the disk is never consulted, so a `..` folded through a symlink names a different entry
    /// than the system would.
    public static func lexicallyNormalize(_ text: [UInt8]) throws(FileSystemError) -> [UInt8] {
        try measuredAnswer(.pathLexicallyNormalize, refusedText: text) { buffer, capacity, length, coreError in
            text.withUnsafeBufferPointer { bytes in
                bc_file_system_path_lexically_normalize(
                    bytes.baseAddress, bytes.count, buffer, capacity, &length, &coreError)
            }
        }
    }

    /// `relative`, text with no leading slash, joined to `base` and normalised: how a composition root turns a
    /// command-line argument into an absolute path against the working directory it was handed.
    public static func resolve(_ relative: [UInt8], against base: AbsolutePath) throws(FileSystemError)
        -> AbsolutePath
    {
        let baseBytes = base.bytes
        let resolved = try measuredAnswer(.pathResolveAgainst, refusedText: relative) {
            buffer, capacity, length, coreError in
            baseBytes.withUnsafeBufferPointer { baseBuffer in
                relative.withUnsafeBufferPointer { relativeBuffer in
                    bc_file_system_path_resolve_against(
                        baseBuffer.baseAddress, baseBuffer.count, relativeBuffer.baseAddress, relativeBuffer.count,
                        buffer, capacity, &length, &coreError)
                }
            }
        }
        return try AbsolutePath(validatingBytes: resolved)
    }

    /// The library a component names when it spells one the way a platform does (`libX.so`, `libX.so.1.2`,
    /// `libX.dylib`, `libX.a`, `X.framework`, `X.dll`, byte for byte), or nil.
    public static func libraryName(of component: [UInt8]) throws(FileSystemError) -> LibraryName? {
        var kind: Int32 = 0
        var stemOffset = 0
        var stemLength = 0
        try callPathModule(.pathLibraryName, refusedText: component) { coreError in
            component.withUnsafeBufferPointer { bytes in
                bc_file_system_path_library_name(
                    bytes.baseAddress, bytes.count, &kind, &stemOffset, &stemLength, &coreError)
            }
        }
        if kind == Int32(bc_file_system_library_kind_none.rawValue) {
            return nil
        }
        guard let libraryKind = LibraryKind(rawValue: kind) else {
            throw FileSystemError.answerOutsideEnum(called: .pathLibraryName, locationText: component)
        }
        let stem = try located(in: component, offset: stemOffset, length: stemLength, called: .pathLibraryName)
        return LibraryName(kind: libraryKind, stem: stem)
    }

    // MARK: - Shared by the typed overloads

    private static func joinedText(_ base: [UInt8], _ relative: [UInt8]) throws(FileSystemError) -> [UInt8] {
        try measuredAnswer(.pathJoin, refusedText: base) { buffer, capacity, length, coreError in
            base.withUnsafeBufferPointer { baseBuffer in
                relative.withUnsafeBufferPointer { relativeBuffer in
                    bc_file_system_path_join(
                        baseBuffer.baseAddress, baseBuffer.count, relativeBuffer.baseAddress, relativeBuffer.count,
                        buffer, capacity, &length, &coreError)
                }
            }
        }
    }

    /// The parent as a prefix of `path`, or nil when there is none.
    private static func parentText(of path: [UInt8]) throws(FileSystemError) -> [UInt8]? {
        var present: Int32 = 0
        var parentLength = 0
        try callPathModule(.pathParent, refusedText: path) { coreError in
            path.withUnsafeBufferPointer { bytes in
                bc_file_system_path_parent(bytes.baseAddress, bytes.count, &present, &parentLength, &coreError)
            }
        }
        if present == 0 {
            return nil
        }
        return try located(in: path, offset: 0, length: parentLength, called: .pathParent)
    }

    private static func lastComponentText(of path: [UInt8]) throws(FileSystemError) -> [UInt8] {
        var offset = 0
        var componentLength = 0
        try callPathModule(.pathLastComponent, refusedText: path) { coreError in
            path.withUnsafeBufferPointer { bytes in
                bc_file_system_path_last_component(
                    bytes.baseAddress, bytes.count, &offset, &componentLength, &coreError)
            }
        }
        return try located(in: path, offset: offset, length: componentLength, called: .pathLastComponent)
    }

    /// The components through the module's iterator: `cursor` starts at 0 and the core advances it with each
    /// call, until a call answers that nothing is left.
    private static func componentTexts(of path: [UInt8]) throws(FileSystemError) -> [[UInt8]] {
        var found: [[UInt8]] = []
        var cursor = 0
        while true {
            var offset = 0
            var componentLength = 0
            var present: Int32 = 0
            try callPathModule(.pathNextComponent, refusedText: path) { coreError in
                path.withUnsafeBufferPointer { bytes in
                    bc_file_system_path_next_component(
                        bytes.baseAddress, bytes.count, &cursor, &offset, &componentLength, &present, &coreError)
                }
            }
            if present == 0 {
                return found
            }
            found.append(try located(in: path, offset: offset, length: componentLength, called: .pathNextComponent))
        }
    }
}

// MARK: - Crossing to C

/// One call into the path module: `body` gets the error to fill and answers the core's status; a non-zero status
/// becomes the thrown error, with `refusedText` as its path (the module leaves the error's name empty for a
/// refusal of text, so the path is exactly the bytes the call was handed).
private func callPathModule(
    _ called: Operation,
    refusedText: [UInt8],
    _ body: (inout bc_file_system_error) -> Int32
) throws(FileSystemError) {
    var coreError = bc_file_system_error()
    if body(&coreError) != 0 {
        throw FileSystemError(core: coreError, called: called, locationText: refusedText)
    }
}

/// The buffer protocol for an answer of the path module, in two calls: the size first (no buffer and a capacity
/// of 0, which the core answers with `bufferTooSmall` and the length it needs), then a buffer of exactly that
/// size. A path answer depends on nothing but the call's arguments, so it cannot grow between the two calls, and
/// the operations' bounded grow loop has nothing to absorb here. `body` is the core's function with everything but
/// the buffer bound.
private func measuredAnswer(
    _ called: Operation,
    refusedText: [UInt8],
    _ body: (UnsafeMutablePointer<UInt8>?, Int, inout Int, inout bc_file_system_error) -> Int32
) throws(FileSystemError) -> [UInt8] {
    let bufferTooSmall = Int32(bc_file_system_error_kind_buffer_too_small.rawValue)
    var coreError = bc_file_system_error()
    var needed = 0
    let measured = body(nil, 0, &needed, &coreError)
    if measured == 0 {
        // Only an empty answer fits in no buffer at all.
        return []
    }
    if measured != bufferTooSmall {
        throw FileSystemError(core: coreError, called: called, locationText: refusedText)
    }
    var answer = [UInt8](repeating: 0, count: needed)
    var length = 0
    let stored = answer.withUnsafeMutableBufferPointer { buffer in
        body(buffer.baseAddress, buffer.count, &length, &coreError)
    }
    if stored != 0 {
        throw FileSystemError(core: coreError, called: called, locationText: refusedText)
    }
    return Array(answer.prefix(length))
}

/// The bytes the core located inside `text`. A location that falls outside the text is answered with the face's
/// stand-in error rather than trusted or trapped on, as an enum value outside its enum is: a core and a face built
/// together cannot produce one.
private func located(in text: [UInt8], offset: Int, length: Int, called: Operation) throws(FileSystemError)
    -> [UInt8]
{
    guard offset >= 0, length >= 0, offset <= text.count, length <= text.count - offset else {
        throw FileSystemError.answerOutsideEnum(called: called, locationText: text)
    }
    return Array(text[offset..<(offset + length)])
}
