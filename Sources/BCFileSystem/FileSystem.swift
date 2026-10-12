import CBCFileSystem

// MARK: - Path values

private let slash = UInt8(ascii: "/")

/// Path text relative to a directory, as bytes, one spelling per path so equality is a byte compare.
///
/// Validated once at construction by the lexical rules the path module will own (stage `path-module` moves
/// them into C; until then they live here, minimal): not empty, no NUL byte, no leading slash, no empty
/// component (`a//b`, a trailing slash) and no `..` component. Nothing on disk is consulted.
public struct RelativePath: Equatable, Sendable {
    public let bytes: [UInt8]

    public init(validating text: String) throws(FileSystemError) {
        try self.init(validatingBytes: Array(text.utf8))
    }

    public init(validatingBytes bytes: [UInt8]) throws(FileSystemError) {
        let refused = FileSystemError.refusedPath(called: .relativePathFrom, bytes: bytes)
        if bytes.isEmpty || bytes[0] == slash || bytes.contains(0) {
            throw refused
        }
        for component in bytes.split(separator: slash, omittingEmptySubsequences: false) {
            if component.isEmpty || component.elementsEqual("..".utf8) {
                throw refused
            }
        }
        self.bytes = bytes
    }

    /// The bytes as text, for display.
    public var text: String {
        String(decoding: bytes, as: UTF8.self)
    }
}

/// Absolute path text, as bytes. `..` is allowed and reported by the path module, never resolved here.
///
/// Validated once at construction: starts with a slash, no NUL byte.
public struct AbsolutePath: Equatable, Sendable {
    public let bytes: [UInt8]

    public init(validating text: String) throws(FileSystemError) {
        try self.init(validatingBytes: Array(text.utf8))
    }

    public init(validatingBytes bytes: [UInt8]) throws(FileSystemError) {
        if bytes.isEmpty || bytes[0] != slash || bytes.contains(0) {
            throw FileSystemError.refusedPath(called: .absolutePathFrom, bytes: bytes)
        }
        self.bytes = bytes
    }

    /// The bytes as text, for display.
    public var text: String {
        String(decoding: bytes, as: UTF8.self)
    }
}

extension FileSystemError {
    static func refusedPath(called: Operation, bytes: [UInt8]) -> FileSystemError {
        FileSystemError(
            kind: .invalidArgument,
            operation: called,
            step: nil,
            systemCode: 0,
            path: String(decoding: bytes, as: UTF8.self),
            nameTruncated: false,
            failedLocation: nil
        )
    }
}

/// Where an operation acts: an absolute path the system resolves, or a relative path beneath a `Directory`
/// with containment enforced by the kernel. One sum type, so every operation has one signature. Not Sendable:
/// `beneath` borrows a directory handle, which the core uses from one thread at a time.
public enum Location {
    case at(AbsolutePath)
    /// `directory` must outlive the location.
    case beneath(Directory, RelativePath)

    /// The text this location stands for, as the face reports it: the absolute bytes, or the directory's
    /// opened-as text, a slash and the relative bytes.
    public var text: [UInt8] {
        switch self {
        case .at(let path):
            return path.bytes
        case .beneath(let directory, let relative):
            return directory.openedAsText + [slash] + relative.bytes
        }
    }

    /// Lends the C struct, borrowing this location's bytes, for one call. `package`, for the binding suite's
    /// round trip.
    package func withCore<T>(_ body: (UnsafePointer<bc_file_system_location>) -> T) -> T {
        switch self {
        case .at(let path):
            return path.bytes.withUnsafeBufferPointer { buffer in
                var location = bc_file_system_location_at(buffer.baseAddress, buffer.count)
                return withUnsafePointer(to: &location) { body($0) }
            }
        case .beneath(let directory, let relative):
            return relative.bytes.withUnsafeBufferPointer { buffer in
                var location = bc_file_system_location_beneath(directory.handle, buffer.baseAddress, buffer.count)
                return withUnsafePointer(to: &location) { body($0) }
            }
        }
    }
}

// MARK: - The file system

/// The file system a piece of code is handed. Holds the engine it runs in; each method is one call into the C
/// core plus the answer's conversion, and nothing here is a protocol: the double lives below the core, behind
/// the engine's observer, so there is nothing to substitute on this side.
///
/// Every operation states everything it needs: there is no default durability, no default mode and no
/// convenience overload. Every operation throws `FileSystemError`, with the full path composed from the
/// `Location` given.
public struct FileSystem: Sendable {
    public let engine: Engine

    public init(engine: Engine) {
        self.engine = engine
    }

    /// One call into the core over one location: `body` gets the engine's handle, the C location and the
    /// error to fill, and answers the core's status; a non-zero status becomes the thrown error, with the
    /// location's text as the path.
    private func call(
        _ called: Operation,
        _ location: Location,
        _ body: (OpaquePointer, UnsafePointer<bc_file_system_location>, inout bc_file_system_error) -> Int32
    ) throws(FileSystemError) {
        var coreError = bc_file_system_error()
        let outcome = engine.withHandle { handle in
            location.withCore { coreLocation in
                body(handle, coreLocation, &coreError)
            }
        }
        if outcome != 0 {
            throw FileSystemError(core: coreError, called: called, locationText: location.text)
        }
    }

    /// The same over two locations; an error names the end it belongs to (`FileSystemError.failedLocation`).
    private func call(
        _ called: Operation,
        from source: Location,
        to destination: Location,
        _ body: (
            OpaquePointer, UnsafePointer<bc_file_system_location>, UnsafePointer<bc_file_system_location>,
            inout bc_file_system_error
        ) -> Int32
    ) throws(FileSystemError) {
        var coreError = bc_file_system_error()
        let outcome = engine.withHandle { handle in
            source.withCore { coreSource in
                destination.withCore { coreDestination in
                    body(handle, coreSource, coreDestination, &coreError)
                }
            }
        }
        if outcome != 0 {
            throw FileSystemError(
                core: coreError, called: called, firstLocationText: source.text, secondLocationText: destination.text)
        }
    }

    /// The durability a call achieved, from the integer it stored.
    private func durability(achieved: Int32, called: Operation, locationText: [UInt8]) throws(FileSystemError)
        -> Durability
    {
        guard let durability = Durability(rawValue: achieved) else {
            throw FileSystemError.answerOutsideEnum(called: called, locationText: locationText)
        }
        return durability
    }

    /// Opens the directory at `location`, remembering its text; something that is not a directory is
    /// `notDirectory`; where the platform cannot enforce containment beneath it, `unsupported`.
    public func openDirectory(_ location: Location, finalSymlink: FinalSymlink) throws(FileSystemError) -> Directory {
        var created: OpaquePointer?
        try call(.openDirectory, location) { handle, coreLocation, coreError in
            bc_file_system_directory_open(handle, coreLocation, finalSymlink.rawValue, &created, &coreError)
        }
        guard let handle = created else {
            throw FileSystemError.answerOutsideEnum(called: .openDirectory, locationText: location.text)
        }
        return Directory(adopting: handle, openedAsText: location.text)
    }

    /// What is at `location`, or nil when nothing is: absence is a value here, not an error.
    public func status(_ location: Location, finalSymlink: FinalSymlink) throws(FileSystemError) -> Status? {
        var present: Int32 = 0
        var coreStatus = bc_file_system_status()
        try call(.status, location) { handle, coreLocation, coreError in
            bc_file_system_status_of(handle, coreLocation, finalSymlink.rawValue, &present, &coreStatus, &coreError)
        }
        if present == 0 {
            return nil
        }
        guard let status = Status(core: coreStatus) else {
            throw FileSystemError.answerOutsideEnum(called: .status, locationText: location.text)
        }
        return status
    }

    /// The bytes of a regular file, under the read limit. Anything that is not a regular file is
    /// `notRegularFile`. The buffer protocol is absorbed here: at most three attempts, then the error as
    /// received.
    public func readFile(_ location: Location, options: ReadOptions) throws(FileSystemError) -> [UInt8] {
        let coreOptions = options.core
        // Under a bound no successful answer is longer than the bound, so a smaller bound is offered whole.
        let firstCapacity: Int
        switch options.limit {
        case .wholeFile:
            firstCapacity = firstReadCapacity
        case .atMost(let bytes):
            firstCapacity = Int(min(bytes, UInt64(firstReadCapacity)))
        }
        return try grownAnswer(firstCapacity: firstCapacity, operation: .readFile, locationText: location.text) {
            buffer, capacity, length, coreError in
            engine.withHandle { handle in
                location.withCore { coreLocation in
                    withUnsafePointer(to: coreOptions) { optionsPointer in
                        bc_file_system_read_file(
                            handle, coreLocation, optionsPointer, buffer, capacity, &length, &coreError)
                    }
                }
            }
        }
    }

    /// Replaces or creates the regular file at `location` with `bytes`, atomically: a reader sees the old
    /// content or the new, never a mixture. The directory must exist. Returns the durability achieved.
    public func writeFile(_ location: Location, bytes: [UInt8], options: WriteOptions) throws(FileSystemError)
        -> Durability
    {
        let coreOptions = options.core
        var achieved: Int32 = 0
        try call(.writeFile, location) { handle, coreLocation, coreError in
            bytes.withUnsafeBufferPointer { buffer in
                withUnsafePointer(to: coreOptions) { optionsPointer in
                    bc_file_system_write_file(
                        handle, coreLocation, buffer.baseAddress, buffer.count, optionsPointer, &achieved, &coreError)
                }
            }
        }
        return try durability(achieved: achieved, called: .writeFile, locationText: location.text)
    }

    /// Adds `bytes` to the end of the file at `location`, creating it with `permissions` when it is absent.
    /// Not atomic: after an error, some leading part of the bytes may be in the file.
    public func append(
        _ location: Location, bytes: [UInt8], durability: Durability, permissions: Permissions
    ) throws(FileSystemError) -> Durability {
        let corePermissions = permissions.core
        var achieved: Int32 = 0
        try call(.append, location) { handle, coreLocation, coreError in
            bytes.withUnsafeBufferPointer { buffer in
                withUnsafePointer(to: corePermissions) { permissionsPointer in
                    bc_file_system_append(
                        handle, coreLocation, buffer.baseAddress, buffer.count, durability.rawValue,
                        permissionsPointer, &achieved, &coreError)
                }
            }
        }
        return try self.durability(achieved: achieved, called: .append, locationText: location.text)
    }

    /// Pushes the file or directory at `location` to stable storage; a volume with no sync answers `written`.
    public func sync(_ location: Location) throws(FileSystemError) -> Durability {
        var achieved: Int32 = 0
        try call(.sync, location) { handle, coreLocation, coreError in
            bc_file_system_sync(handle, coreLocation, &achieved, &coreError)
        }
        return try durability(achieved: achieved, called: .sync, locationText: location.text)
    }

    /// Creates one directory; one already there is `alreadyExists`; a missing parent is `notFound`.
    public func createDirectory(_ location: Location, permissions: Permissions) throws(FileSystemError) {
        let corePermissions = permissions.core
        try call(.createDirectory, location) { handle, coreLocation, coreError in
            withUnsafePointer(to: corePermissions) { permissionsPointer in
                bc_file_system_create_directory(handle, coreLocation, permissionsPointer, &coreError)
            }
        }
    }

    /// Creates the directory and every missing one on the way; one already there is left as it is.
    public func createDirectories(_ location: Location, permissions: Permissions) throws(FileSystemError) {
        let corePermissions = permissions.core
        try call(.createDirectories, location) { handle, coreLocation, coreError in
            withUnsafePointer(to: corePermissions) { permissionsPointer in
                bc_file_system_create_directories(handle, coreLocation, permissionsPointer, &coreError)
            }
        }
    }

    /// The entries of a directory, sorted by name so a listing is the same on every run; complete or an error.
    public func listDirectory(_ location: Location, finalSymlink: FinalSymlink) throws(FileSystemError) -> [Entry] {
        // The buffer protocol over an array of entries rather than bytes: the same bounded grow loop.
        var capacity = firstEntryCapacity
        var coreError = bc_file_system_error()
        let bufferTooSmall = Int32(bc_file_system_error_kind_buffer_too_small.rawValue)
        for attempt in 1...mostAttempts {
            var coreEntries = [bc_file_system_entry](repeating: bc_file_system_entry(), count: capacity)
            var count = 0
            let outcome = engine.withHandle { handle in
                location.withCore { coreLocation in
                    coreEntries.withUnsafeMutableBufferPointer { buffer in
                        bc_file_system_list_directory(
                            handle, coreLocation, finalSymlink.rawValue, buffer.baseAddress, buffer.count, &count,
                            &coreError)
                    }
                }
            }
            if outcome == 0 {
                var entries: [Entry] = []
                entries.reserveCapacity(count)
                for coreEntry in coreEntries.prefix(count) {
                    guard let entry = Entry(core: coreEntry) else {
                        throw FileSystemError.answerOutsideEnum(called: .listDirectory, locationText: location.text)
                    }
                    entries.append(entry)
                }
                return entries
            }
            if outcome == bufferTooSmall && attempt < mostAttempts {
                capacity = count
                continue
            }
            throw FileSystemError(core: coreError, called: .listDirectory, locationText: location.text)
        }
        throw FileSystemError(core: coreError, called: .listDirectory, locationText: location.text)
    }

    /// Removes the non-directory at `location`; returns whether there was one. A directory is
    /// `notRegularFile`.
    public func removeFile(_ location: Location) throws(FileSystemError) -> Bool {
        var existed: Int32 = 0
        try call(.removeFile, location) { handle, coreLocation, coreError in
            bc_file_system_remove_file(handle, coreLocation, &existed, &coreError)
        }
        return existed != 0
    }

    /// Removes the empty directory at `location`; returns whether there was one.
    public func removeDirectory(_ location: Location) throws(FileSystemError) -> Bool {
        var existed: Int32 = 0
        try call(.removeDirectory, location) { handle, coreLocation, coreError in
            bc_file_system_remove_directory(handle, coreLocation, &existed, &coreError)
        }
        return existed != 0
    }

    /// Removes `location` and everything below it, never following a symlink, never the root of the file
    /// system, never across a device; returns whether there was anything.
    public func removeTree(_ location: Location) throws(FileSystemError) -> Bool {
        var existed: Int32 = 0
        try call(.removeTree, location) { handle, coreLocation, coreError in
            bc_file_system_remove_tree(handle, coreLocation, &existed, &coreError)
        }
        return existed != 0
    }

    /// Creates a directory with a unique name inside `parent` and returns that name, one component.
    public func createTemporaryDirectory(in parent: Location, permissions: Permissions) throws(FileSystemError)
        -> [UInt8]
    {
        let corePermissions = permissions.core
        return try grownAnswer(
            firstCapacity: firstTextCapacity, operation: .createTemporaryDirectory, locationText: parent.text
        ) { buffer, capacity, length, coreError in
            engine.withHandle { handle in
                parent.withCore { coreParent in
                    withUnsafePointer(to: corePermissions) { permissionsPointer in
                        bc_file_system_create_temporary_directory(
                            handle, coreParent, permissionsPointer, buffer, capacity, &length, &coreError)
                    }
                }
            }
        }
    }

    /// Renames `source` to `destination` in one step; see the header for what `replace` replaces. Returns the
    /// durability achieved. An error names the end it belongs to: a missing source names `source`, a destination
    /// that refuses names `destination`.
    public func rename(
        from source: Location, to destination: Location, existingDestination: ExistingDestination,
        durability: Durability
    ) throws(FileSystemError) -> Durability {
        var achieved: Int32 = 0
        try call(.rename, from: source, to: destination) { handle, coreSource, coreDestination, coreError in
            bc_file_system_rename(
                handle, coreSource, coreDestination, existingDestination.rawValue, durability.rawValue, &achieved,
                &coreError)
        }
        return try self.durability(achieved: achieved, called: .rename, locationText: destination.text)
    }

    /// Copies the regular file at `source` to `destination` with the guarantees of `writeFile`. An error names
    /// the end it belongs to, as `rename`'s does.
    public func copyFile(from source: Location, to destination: Location, options: WriteOptions)
        throws(FileSystemError) -> Durability
    {
        let coreOptions = options.core
        var achieved: Int32 = 0
        try call(.copyFile, from: source, to: destination) { handle, coreSource, coreDestination, coreError in
            withUnsafePointer(to: coreOptions) { optionsPointer in
                bc_file_system_copy_file(handle, coreSource, coreDestination, optionsPointer, &achieved, &coreError)
            }
        }
        return try durability(achieved: achieved, called: .copyFile, locationText: destination.text)
    }

    /// Sets the permission bits of the entry at `location` exactly to `mode`.
    public func setPermissions(_ location: Location, mode: UInt32, finalSymlink: FinalSymlink)
        throws(FileSystemError)
    {
        try call(.setPermissions, location) { handle, coreLocation, coreError in
            bc_file_system_set_permissions(handle, coreLocation, mode, finalSymlink.rawValue, &coreError)
        }
    }

    /// Creates a symlink at `link` whose text is `target`, passed to the system unchanged.
    public func createSymlink(target: [UInt8], at link: Location) throws(FileSystemError) {
        try call(.createSymlink, link) { handle, coreLink, coreError in
            target.withUnsafeBufferPointer { buffer in
                bc_file_system_create_symlink(handle, buffer.baseAddress, buffer.count, coreLink, &coreError)
            }
        }
    }

    /// The text of the symlink at `location`.
    public func readSymlink(_ location: Location) throws(FileSystemError) -> [UInt8] {
        try grownAnswer(firstCapacity: firstTextCapacity, operation: .readSymlink, locationText: location.text) {
            buffer, capacity, length, coreError in
            engine.withHandle { handle in
                location.withCore { coreLocation in
                    bc_file_system_read_symlink(handle, coreLocation, buffer, capacity, &length, &coreError)
                }
            }
        }
    }

    /// Tries to take an advisory exclusive lock on the file at `location` without waiting: the lock when it was
    /// acquired, nil when it is held elsewhere.
    public func tryLock(_ location: Location) throws(FileSystemError) -> Lock? {
        var created: OpaquePointer?
        var coreOutcome: Int32 = 0
        try call(.tryLock, location) { handle, coreLocation, coreError in
            bc_file_system_try_lock(handle, coreLocation, &created, &coreOutcome, &coreError)
        }
        guard let outcome = LockOutcome(rawValue: coreOutcome) else {
            throw FileSystemError.answerOutsideEnum(called: .tryLock, locationText: location.text)
        }
        switch outcome {
        case .heldElsewhere:
            return nil
        case .acquired:
            guard let handle = created else {
                throw FileSystemError.answerOutsideEnum(called: .tryLock, locationText: location.text)
            }
            return Lock(adopting: handle)
        }
    }

    /// Renames the tree at `location` to `.bc-condemned-<name>` beside it, for a later sweep, and returns the
    /// condemned name.
    public func condemn(_ location: Location) throws(FileSystemError) -> [UInt8] {
        try grownAnswer(firstCapacity: firstTextCapacity, operation: .condemn, locationText: location.text) {
            buffer, capacity, length, coreError in
            engine.withHandle { handle in
                location.withCore { coreLocation in
                    bc_file_system_condemn(handle, coreLocation, buffer, capacity, &length, &coreError)
                }
            }
        }
    }

    /// Removes every `.bc-condemned-` tree directly inside `parent`.
    public func removeCondemned(in parent: Location) throws(FileSystemError) {
        try call(.removeCondemned, parent) { handle, coreParent, coreError in
            bc_file_system_remove_condemned(handle, coreParent, &coreError)
        }
    }
}
