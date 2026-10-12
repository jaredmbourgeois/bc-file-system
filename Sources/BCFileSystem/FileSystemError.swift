import CBCFileSystem

/// Why an operation failed: the core's fourteen kinds, each a decision a caller can act on, never a passthrough
/// of errno, plus the C face's grow-and-retry signal, which the face's own compositions absorb and hand on only
/// after their bounded retry. The raw values are the header's; `BindingTests` holds the two lists together.
public enum ErrorKind: Int32, CaseIterable, Sendable {
    /// Nothing exists at the location.
    case notFound = 1
    /// Something exists where a regular file is required, and it is not one.
    case notRegularFile = 2
    /// Something exists where a directory is required, and it is not one.
    case notDirectory = 3
    /// EACCES, EPERM and EROFS: a read-only volume refuses the same way a mode does.
    case permissionDenied = 4
    /// The destination holds something the call was told not to replace.
    case alreadyExists = 5
    /// A directory that must be empty to be removed or replaced is not.
    case directoryNotEmpty = 6
    /// A symlink sat where the call was told not to follow one.
    case symlinkRefused = 7
    /// The file is larger than a read limit, than `Int` on this target, or than the process's file-size limit.
    case tooLarge = 8
    /// EXDEV: the two locations of a rename are on different devices.
    case crossDevice = 9
    /// ENOSPC and EDQUOT: the disk or the quota is full. Never memory.
    case noSpace = 10
    /// ENOMEM, EMFILE, ENFILE: memory or descriptors ran out.
    case resourceExhausted = 11
    /// The platform cannot keep the guarantee the call asks for, and the library refuses rather than weakens it.
    case unsupported = 12
    /// A contract violation; nothing was touched.
    case invalidArgument = 13
    /// Any other system failure; `systemCode` says which.
    case ioFailed = 14
    /// The caller's buffer cannot hold the answer. The face's grow loop absorbs this; a caller sees it only
    /// after the loop's three attempts.
    case bufferTooSmall = 15
}

/// Which public function an error came from, in the header's order.
public enum Operation: Int32, CaseIterable, Sendable {
    case engineCreateLive = 1
    case engineCreateObservedForTests = 2
    case openDirectory = 3
    case directoryOpenedAs = 4
    case observedFaultAdd = 5
    case observedFaultsClear = 6
    case observedStepCount = 7
    case status = 8
    case readFile = 9
    case writeFile = 10
    case append = 11
    case sync = 12
    case createDirectory = 13
    case createDirectories = 14
    case listDirectory = 15
    case removeFile = 16
    case removeDirectory = 17
    case removeTree = 18
    case createTemporaryDirectory = 19
    case rename = 20
    case copyFile = 21
    case setPermissions = 22
    case createSymlink = 23
    case readSymlink = 24
    case tryLock = 25
    case condemn = 26
    case removeCondemned = 27
    case relativePathFrom = 28
    case absolutePathFrom = 29
}

/// The system call an operation was making when it failed, and the observer's vocabulary.
public enum Step: Int32, CaseIterable, Sendable {
    case open = 1
    case openat = 2
    case fstat = 3
    case fstatat = 4
    case read = 5
    case pread = 6
    case write = 7
    case fsync = 8
    /// fcntl(F_FULLFSYNC): the durable sync on Apple platforms, where fsync stops at the drive's cache.
    case fcntlFullfsync = 9
    case renameat = 10
    /// The platform's no-replace rename.
    case renameNoReplace = 11
    case unlinkat = 12
    case mkdirat = 13
    case fdopendir = 14
    case readdir = 15
    case symlinkat = 16
    case readlinkat = 17
    case flock = 18
    case fchmod = 19
    case copyRange = 20
    case cloneFile = 21
    case close = 22
    /// The st_dev/st_ino comparison that catches a directory swapped in between a stat and an open.
    case identityCheck = 23
    /// The Linux fast path of a `beneath` location: openat2 over the whole relative path in one call. A scripted
    /// ENOSYS, EINVAL, ENOTSUP or EOPNOTSUPP on this step makes the engine walk the path instead.
    case openat2 = 24
    /// The Apple fast path of a `beneath` location: openat with O_NOFOLLOW_ANY over the whole relative path.
    case openatNofollowAny = 25
    /// fchmodat: the mode set by name under a held directory (AT_SYMLINK_NOFOLLOW for the entry itself).
    case fchmodat = 26
}

/// Why an operation failed. A plain value: the core reports enums and integers; the face composes `path` from
/// the location it holds and renders the text.
public struct FileSystemError: Error, Equatable, Sendable {
    public let kind: ErrorKind
    public let operation: Operation
    /// The system call that failed; nil for a refusal made before any system call.
    public let step: Step?
    /// The errno of the system call that failed, or 0 when the failure was not a system call's.
    public let systemCode: Int32
    /// The full path the failing step acted on: the location's text, then a slash and the entry's name when
    /// the step acted on an entry inside it. Empty for a call that took no location. For reporting, never for
    /// a retry.
    public let path: String
    /// True when the entry's name was longer than the core's bounded name field, so `path` ends early.
    public let nameTruncated: Bool

    /// Public so that a consumer's test can build the error its own double throws.
    public init(
        kind: ErrorKind,
        operation: Operation,
        step: Step?,
        systemCode: Int32,
        path: String,
        nameTruncated: Bool
    ) {
        self.kind = kind
        self.operation = operation
        self.step = step
        self.systemCode = systemCode
        self.path = path
        self.nameTruncated = nameTruncated
    }

    /// "<operation>: <path>: <kind>", with the errno after the kind when a system call failed, and without
    /// the path segment when the call took no location. Rendered with no library call, so it is safe from
    /// any thread.
    public var message: String {
        var text = "\(operation): "
        if !path.isEmpty {
            text += "\(path): "
        }
        text += "\(kind)"
        if systemCode != 0 {
            text += " (errno \(systemCode))"
        }
        return text
    }
}

extension ErrorKind {
    /// The kind the core answered. A core and a face built together cannot disagree (the binding suite pins
    /// every value), so a value outside the enum is never seen in practice; it is still answered as "a system
    /// failure we could not classify" rather than trusted or trapped on.
    static func fromCore(_ value: Int32) -> ErrorKind {
        if let known = ErrorKind(rawValue: value) {
            return known
        }
        return .ioFailed
    }
}

extension FileSystemError {
    /// The face's one conversion of a C error, with the full path composed from the location's text and the
    /// entry name the core reported. The core names the operation itself; `called` is the operation this face
    /// invoked and stands in only if the core's value were outside the enum.
    package init(core: bc_file_system_error, called: Operation, locationText: [UInt8]) {
        var pathBytes = locationText
        let nameLength = Int(core.name_length)
        if nameLength > 0 {
            // The name field is a fixed C array, which Swift imports as a tuple; its bytes are read in place.
            let nameBytes = withUnsafeBytes(of: core.name) { bytes in
                Array(bytes.prefix(min(nameLength, bytes.count)))
            }
            if !pathBytes.isEmpty {
                pathBytes.append(UInt8(ascii: "/"))
            }
            pathBytes += nameBytes
        }
        let operation: Operation
        if let reported = Operation(rawValue: core.operation) {
            operation = reported
        } else {
            operation = called
        }
        self.init(
            kind: ErrorKind.fromCore(core.kind),
            operation: operation,
            step: Step(rawValue: core.step),
            systemCode: core.system_code,
            path: String(decoding: pathBytes, as: UTF8.self),
            nameTruncated: core.name_truncated != 0
        )
    }

    /// The error the face answers when the core's answer holds a value outside one of its enums, which a core
    /// and a face built together cannot produce.
    package static func answerOutsideEnum(called: Operation, locationText: [UInt8]) -> FileSystemError {
        FileSystemError(
            kind: .ioFailed,
            operation: called,
            step: nil,
            systemCode: 0,
            path: String(decoding: locationText, as: UTF8.self),
            nameTruncated: false
        )
    }
}
