import CBCFileSystem

/// What to do when the last component of a path is a symlink: the link itself is the entry, or the operation
/// acts on what it points at. Required wherever both answers are in measured use.
public enum FinalSymlink: Int32, CaseIterable, Sendable {
    case refuse = 1
    case follow = 2
}

/// What to do when a mutation's destination already holds something.
public enum ExistingDestination: Int32, CaseIterable, Sendable {
    case replace = 1
    /// `alreadyExists`, and the destination is untouched.
    case refuse = 2
}

/// How far a change must travel before the call returns. Every write, append, sync, rename and copy is told
/// which one it needs and answers with the one it achieved: once a change is visible, a sync that fails lowers
/// the answer to `written` rather than raising an error, because the change happened.
public enum Durability: Int32, CaseIterable, Sendable {
    /// Handed to the operating system; outlives the process; a power loss can still lose it.
    case written = 1
    /// On stable storage: `F_FULLFSYNC` on Apple platforms, `fsync` on the file and its directory elsewhere.
    case durable = 2
}

/// How much of a file a read may take. There is no default: a caller states one of the two.
public enum ReadLimit: Equatable, Sendable {
    /// Every byte, however many.
    case wholeFile
    /// At most `bytes`; a larger file is `tooLarge`, never a silent prefix.
    case atMost(bytes: UInt64)
}

/// The mode of anything an operation creates or replaces. There is no default mode: a caller states one of the
/// two policies with its mode.
///
/// `mode` is the permission bits as chmod takes them (`0o644`), applied exactly: the process umask is ambient
/// state the library does not consult. What already exists and is not replaced is never touched.
public enum Permissions: Equatable, Sendable {
    /// A created file or directory gets `mode`, and so does a file that replaces an existing one.
    case exactly(mode: UInt32)
    /// A file that replaces an existing one keeps the mode the replaced file had, so an operator's chmod
    /// survives a rewrite; anything newly created gets `mode`.
    case preserveOr(mode: UInt32)
}

/// Every choice a write, a copy or a replace makes, each one required. Built once as a named value at each
/// composition root; a field added later fails to compile at every root, which is the point.
public struct WriteOptions: Equatable, Sendable {
    public let durability: Durability
    public let permissions: Permissions
    public let existingDestination: ExistingDestination
    public let finalSymlink: FinalSymlink

    public init(
        durability: Durability,
        permissions: Permissions,
        existingDestination: ExistingDestination,
        finalSymlink: FinalSymlink
    ) {
        self.durability = durability
        self.permissions = permissions
        self.existingDestination = existingDestination
        self.finalSymlink = finalSymlink
    }
}

/// Every choice a read makes, each one required.
public struct ReadOptions: Equatable, Sendable {
    public let limit: ReadLimit
    public let finalSymlink: FinalSymlink

    public init(limit: ReadLimit, finalSymlink: FinalSymlink) {
        self.limit = limit
        self.finalSymlink = finalSymlink
    }
}

// MARK: - Crossing to C

// Each value type crosses to its C struct in one place, so the field order lives in one place.

extension ReadLimit {
    package var core: bc_file_system_read_limit {
        switch self {
        case .wholeFile:
            return bc_file_system_read_limit(
                kind: Int32(bc_file_system_read_limit_kind_whole_file.rawValue), at_most: 0)
        case .atMost(let bytes):
            return bc_file_system_read_limit(
                kind: Int32(bc_file_system_read_limit_kind_at_most.rawValue), at_most: bytes)
        }
    }
}

extension Permissions {
    package var core: bc_file_system_permissions {
        switch self {
        case .exactly(let mode):
            return bc_file_system_permissions(
                policy: Int32(bc_file_system_permissions_policy_exactly.rawValue), mode: mode)
        case .preserveOr(let mode):
            return bc_file_system_permissions(
                policy: Int32(bc_file_system_permissions_policy_preserve_or.rawValue), mode: mode)
        }
    }
}

extension WriteOptions {
    package var core: bc_file_system_write_options {
        bc_file_system_write_options(
            durability: durability.rawValue,
            permissions: permissions.core,
            existing_destination: existingDestination.rawValue,
            final_symlink: finalSymlink.rawValue
        )
    }
}

extension ReadOptions {
    package var core: bc_file_system_read_options {
        bc_file_system_read_options(limit: limit.core, final_symlink: finalSymlink.rawValue)
    }
}
