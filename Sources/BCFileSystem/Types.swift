import CBCFileSystem

/// What an entry is, judged without following a symlink.
public enum EntryType: Int32, CaseIterable, Sendable {
    case regularFile = 1
    case directory = 2
    /// Reported, never followed: a link resolves to whatever it points at, and the library does not go there.
    case symlink = 3
    /// A socket, a FIFO, a device.
    case other = 4
}

/// What the file system records about one entry.
public struct Status: Equatable, Sendable {
    public let entryType: EntryType
    /// A regular file's size in bytes; 0 for every other type.
    public let size: Int64
    /// The permission bits (`0o644`), without the file-type bits.
    public let mode: UInt32
    /// st_dev and st_ino: the entry's identity, which `removeTree` and the resolution layer compare.
    public let device: UInt64
    public let inode: UInt64
    public let linkCount: UInt64

    public init(entryType: EntryType, size: Int64, mode: UInt32, device: UInt64, inode: UInt64, linkCount: UInt64) {
        self.entryType = entryType
        self.size = size
        self.mode = mode
        self.device = device
        self.inode = inode
        self.linkCount = linkCount
    }
}

/// One entry of a directory listing: its name, one component, as the bytes the system holds, and what it is.
public struct Entry: Equatable, Sendable {
    public let entryType: EntryType
    public let name: [UInt8]

    public init(entryType: EntryType, name: [UInt8]) {
        self.entryType = entryType
        self.name = name
    }

    /// The name as text, for display; a name that is not UTF-8 is shown with replacement characters.
    public var nameText: String {
        String(decoding: name, as: UTF8.self)
    }
}

/// The answer of `tryLock`.
public enum LockOutcome: Int32, CaseIterable, Sendable {
    case acquired = 1
    /// Another process or descriptor holds the lock; nothing was waited for.
    case heldElsewhere = 2
}

/// An advisory lock held on a file, let go when the last reference goes away. The core uses a lock handle from
/// one thread at a time, so this is not Sendable. Taken only by `FileSystem.tryLock`.
public final class Lock {
    private let handle: OpaquePointer

    init(adopting handle: OpaquePointer) {
        self.handle = handle
    }

    deinit {
        bc_file_system_lock_release(handle)
    }
}

/// An open directory the kernel enforces containment beneath, which remembers the text it was opened with so
/// that an error beneath it reads as that text, a slash and the relative path. Released when the last
/// reference goes away. The core uses a directory handle from one thread at a time, so this is not Sendable.
/// Opened only by `FileSystem.openDirectory`.
public final class Directory {
    let handle: OpaquePointer
    /// The text the directory was opened with, as the face composed it for the location it opened: the same
    /// bytes the core remembers, kept here so error paths compose without a C call.
    public let openedAsText: [UInt8]

    init(adopting handle: OpaquePointer, openedAsText: [UInt8]) {
        self.handle = handle
        self.openedAsText = openedAsText
    }

    deinit {
        bc_file_system_directory_release(handle)
    }

    /// The text the directory was opened with, read from the core through the buffer protocol (the binding's
    /// call; `openedAsText` is the face's copy of the same bytes).
    public func openedAs() throws(FileSystemError) -> [UInt8] {
        try grownAnswer(firstCapacity: firstTextCapacity, operation: .directoryOpenedAs, locationText: openedAsText) {
            buffer, capacity, length, coreError in
            bc_file_system_directory_opened_as(handle, buffer, capacity, &length, &coreError)
        }
    }
}

// MARK: - Crossing from C

extension Status {
    /// The status the core answered, or nil when its entry type is outside the enum.
    package init?(core: bc_file_system_status) {
        guard let entryType = EntryType(rawValue: core.entry_type) else {
            return nil
        }
        self.init(
            entryType: entryType,
            size: core.size,
            mode: core.mode,
            device: core.device,
            inode: core.inode,
            linkCount: core.link_count
        )
    }

    package var core: bc_file_system_status {
        bc_file_system_status(
            entry_type: entryType.rawValue,
            size: size,
            mode: mode,
            device: device,
            inode: inode,
            link_count: linkCount
        )
    }
}

extension Entry {
    /// The entry the core answered, or nil when its type is outside the enum or its length impossible.
    package init?(core: bc_file_system_entry) {
        guard let entryType = EntryType(rawValue: core.entry_type) else {
            return nil
        }
        let nameLength = Int(core.name_length)
        let name: [UInt8]? = withUnsafeBytes(of: core.name) { bytes in
            nameLength <= bytes.count ? Array(bytes.prefix(nameLength)) : nil
        }
        guard let name else {
            return nil
        }
        self.init(entryType: entryType, name: name)
    }
}
