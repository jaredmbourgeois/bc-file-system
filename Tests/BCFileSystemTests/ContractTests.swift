import BCFileSystem
import BCFileSystemTestFixtures
import CBCFileSystem
import Testing

// The errno values the cases name (ENOENT, EACCES, ...) come from the platform's C library.
#if canImport(Darwin)
    import Darwin
#else
    import Glibc
#endif

/// The contract of the Swift face, through the C core, in a directory the test created: the cases of the C++
/// contract suite the Swift face reaches, each one checked through the fixtures' raw helpers. The ones it cannot
/// reach (the disk filling up mid-write, which system call a durable write syncs with) are scripted through the
/// observer and proven in the C++ suite.
///
/// Every case that needs the engine is disabled with the stage that turns it on; the core is an honest stub
/// until then. A case of a permission failure returns early where the process reads what its permission bits
/// forbid (the superuser), because no such failure can happen there.
struct ContractTests {
    // The options every case states, built once here so a field added later fails in one place.
    static let wholeFileRefusingSymlink = ReadOptions(limit: .wholeFile, finalSymlink: .refuse)
    static let writtenExactly0644 = WriteOptions(
        durability: .written, permissions: .exactly(mode: 0o644), existingDestination: .replace, finalSymlink: .refuse)
    static let writtenExactly0600 = WriteOptions(
        durability: .written, permissions: .exactly(mode: 0o600), existingDestination: .replace, finalSymlink: .refuse)
    static let writtenPreserveOr0600 = WriteOptions(
        durability: .written, permissions: .preserveOr(mode: 0o600), existingDestination: .replace,
        finalSymlink: .refuse)
    static let durableExactly0640 = WriteOptions(
        durability: .durable, permissions: .exactly(mode: 0o640), existingDestination: .replace, finalSymlink: .refuse)
    static let durableExactly0644 = WriteOptions(
        durability: .durable, permissions: .exactly(mode: 0o644), existingDestination: .replace, finalSymlink: .refuse)

    /// A file system over an observed engine, in a fresh directory of its own.
    private func observedFileSystem() throws -> ObservedFileSystem {
        try FileSystem.observed(root: TemporaryDirectory.create())
    }

    /// The directory itself, as a location.
    private func rootLocation(of observed: ObservedFileSystem) throws(FileSystemError) -> Location {
        .at(try AbsolutePath(validating: observed.root.path))
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

    // MARK: - Read and write

    @Test(.disabled("stage 14 writes-in-c"))
    func writeThenReadReturnsTheSameBytes() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("payload.bin")
        let bytes: [UInt8] = [0, 1, 2, 254, 255]

        let achieved = try observed.fileSystem.writeFile(location, bytes: bytes, options: Self.durableExactly0640)

        #expect(achieved == .durable)
        #expect(try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink) == bytes)
        #expect(try observed.root.rawMode(of: "payload.bin") == 0o640)
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func anEmptyFileRoundTrips() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("empty")

        _ = try observed.fileSystem.writeFile(location, bytes: [], options: Self.writtenExactly0600)

        #expect(try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink) == [])
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func aFileLargerThanAnyInternalBufferRoundTrips() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("large.bin")
        let bytes = [UInt8](repeating: 0x5A, count: 3 * 1024 * 1024)

        _ = try observed.fileSystem.writeFile(location, bytes: bytes, options: Self.writtenExactly0644)

        #expect(try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink) == bytes)
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func writeReplacesTheContentAndLeavesNoTemporaryFile() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("config")
        try observed.root.rawWrite("old content, and longer than the new", to: "config", mode: 0o644)

        _ = try observed.fileSystem.writeFile(location, bytes: Array("new".utf8), options: Self.writtenExactly0644)

        #expect(try observed.root.rawRead("config") == "new")
        #expect(try observed.root.rawNames() == ["config"])
    }

    /// A read that starts with a buffer too small (the face offers 64 KiB first) is asked again with the size
    /// the core reports.
    @Test
    func aReadGrowsItsBufferUntilTheFileFits() throws {
        let observed = try observedFileSystem()
        let text = String(repeating: "longer than the first buffer\n", count: 4 * 1024)
        try observed.root.rawWrite(text, to: "notes.txt", mode: 0o644)

        let bytes = try observed.fileSystem.readFile(
            try observed.location("notes.txt"), options: Self.wholeFileRefusingSymlink)

        #expect(String(decoding: bytes, as: UTF8.self) == text)
    }

    // MARK: - Not found

    @Test
    func readOfNothingIsNotFound() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("absent.txt")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink)
        }

        #expect(error?.kind == .notFound)
        #expect(error?.systemCode == ENOENT)
        #expect(error?.path == observed.root.absolutePath(of: "absent.txt"))
    }

    @Test
    func statusOfNothingIsNilNotAnError() throws {
        let observed = try observedFileSystem()

        #expect(try observed.fileSystem.status(try observed.location("absent.txt"), finalSymlink: .refuse) == nil)
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func writeIntoADirectoryThatIsNotThereIsNotFound() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("missing/file")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.writeFile(location, bytes: [1], options: Self.writtenExactly0644)
        }

        #expect(error?.kind == .notFound)
    }

    @Test(.disabled("stage 15 directories-in-c"))
    func listOfNothingIsNotFound() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("absent")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.listDirectory(location, finalSymlink: .refuse)
        }

        #expect(error?.kind == .notFound)
    }

    // MARK: - Not a regular file, and a refused symlink

    @Test
    func readRefusesASymlinkEvenToARegularFile() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("behind the link", to: "target.txt", mode: 0o644)
        try observed.root.rawMakeSymlink(at: "link", pointingAt: observed.root.absolutePath(of: "target.txt"))
        let link = try observed.location("link")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.readFile(link, options: Self.wholeFileRefusingSymlink)
        }

        #expect(error?.kind == .symlinkRefused)
    }

    @Test
    func readRefusesADirectory() throws {
        let observed = try observedFileSystem()
        let root = try rootLocation(of: observed)

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.readFile(root, options: Self.wholeFileRefusingSymlink)
        }

        #expect(error?.kind == .notRegularFile)
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func writeRefusesADestinationThatIsASymlink() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("behind the link", to: "target.txt", mode: 0o644)
        try observed.root.rawMakeSymlink(at: "link", pointingAt: observed.root.absolutePath(of: "target.txt"))
        let link = try observed.location("link")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.writeFile(link, bytes: [1], options: Self.writtenExactly0644)
        }

        #expect(error?.kind == .symlinkRefused)
        #expect(try observed.root.rawRead("target.txt") == "behind the link")
    }

    @Test(.disabled("stage 15 directories-in-c"))
    func listOfAFileIsNotDirectory() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "file", mode: 0o644)
        let location = try observed.location("file")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.listDirectory(location, finalSymlink: .refuse)
        }

        #expect(error?.kind == .notDirectory)
    }

    // MARK: - Permission denied

    @Test
    func readWithoutPermissionIsPermissionDenied() throws {
        let observed = try observedFileSystem()
        guard try observed.root.permissionsAreEnforced() else {
            return  // this process reads what its permission bits forbid; no such failure can happen
        }
        try observed.root.rawWrite("no access", to: "secret", mode: 0o000)
        let location = try observed.location("secret")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink)
        }

        #expect(error?.kind == .permissionDenied)
        #expect(error?.systemCode == EACCES)
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func writeIntoADirectoryWithoutPermissionIsPermissionDenied() throws {
        let observed = try observedFileSystem()
        guard try observed.root.permissionsAreEnforced() else {
            return  // this process writes where its permission bits forbid; no such failure can happen
        }
        try observed.root.rawMakeDirectory("read-only", mode: 0o500)
        let location = try observed.location("read-only/file")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.writeFile(location, bytes: [1], options: Self.writtenExactly0644)
        }

        #expect(error?.kind == .permissionDenied)
    }

    // MARK: - Durability

    /// A directory sync that fails after the rename is not an error: the file is already replaced, and the call
    /// reports the durability it did achieve. Files can be created in a directory with mode 0o300, but the
    /// directory cannot be opened for the sync.
    @Test(.disabled("stage 14 writes-in-c"))
    func durableWriteIntoADirectoryThatCannotBeOpenedForItsSyncReportsWritten() throws {
        let observed = try observedFileSystem()
        guard try observed.root.permissionsAreEnforced() else {
            return  // this process opens what its permission bits forbid; no such failure can happen
        }
        try observed.root.rawMakeDirectory("write-only", mode: 0o300)
        let location = try observed.location("write-only/file")

        let achieved = try observed.fileSystem.writeFile(
            location, bytes: Array("content".utf8), options: Self.durableExactly0644)

        #expect(achieved == .written)
        try observed.root.rawSetMode(0o700, of: "write-only")
        #expect(try observed.root.rawRead("write-only/file") == "content")
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func aWrittenWriteReportsWritten() throws {
        let observed = try observedFileSystem()

        let achieved = try observed.fileSystem.writeFile(
            try observed.location("file"), bytes: [1], options: Self.writtenExactly0644)

        #expect(achieved == .written)
    }

    // MARK: - Permissions

    @Test(.disabled("stage 14 writes-in-c"))
    func aReplacedFileKeepsItsModeUnderPreserveOr() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("config")
        try observed.root.rawWrite("old", to: "config", mode: 0o640)

        _ = try observed.fileSystem.writeFile(location, bytes: Array("new".utf8), options: Self.writtenPreserveOr0600)

        #expect(try observed.root.rawRead("config") == "new")
        #expect(try observed.root.rawMode(of: "config") == 0o640)
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func aNewFileGetsTheStatedModeUnderPreserveOr() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("config")

        _ = try observed.fileSystem.writeFile(location, bytes: Array("new".utf8), options: Self.writtenPreserveOr0600)

        #expect(try observed.root.rawMode(of: "config") == 0o600)
    }

    @Test(.disabled("stage 14 writes-in-c"))
    func aReplacedFileGetsTheStatedModeUnderExactly() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("config")
        try observed.root.rawWrite("old", to: "config", mode: 0o600)

        _ = try observed.fileSystem.writeFile(location, bytes: Array("new".utf8), options: Self.writtenExactly0644)

        #expect(try observed.root.rawMode(of: "config") == 0o644)
    }

    // MARK: - Status

    @Test
    func statusReportsAFileADirectoryAndASymlink() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("seven b", to: "file", mode: 0o640)
        try observed.root.rawMakeDirectory("folder", mode: 0o750)
        try observed.root.rawMakeSymlink(at: "link", pointingAt: observed.root.absolutePath(of: "file"))

        let file = try #require(try observed.fileSystem.status(try observed.location("file"), finalSymlink: .refuse))
        let folder = try #require(
            try observed.fileSystem.status(try observed.location("folder"), finalSymlink: .refuse))
        let link = try #require(try observed.fileSystem.status(try observed.location("link"), finalSymlink: .refuse))

        #expect(file.entryType == .regularFile)
        #expect(file.size == 7)
        #expect(file.mode == 0o640)
        #expect(file.linkCount == 1)
        #expect(folder.entryType == .directory)
        #expect(folder.size == 0)
        #expect(folder.mode == 0o750)
        #expect(link.entryType == .symlink)
    }

    // MARK: - Listing

    @Test(.disabled("stage 15 directories-in-c"))
    func listReturnsTheEntriesSortedEachWithItsType() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("alpha", to: "a.txt", mode: 0o644)
        try observed.root.rawMakeDirectory("sub", mode: 0o755)
        try observed.root.rawWrite("bee", to: "sub/b.txt", mode: 0o600)
        try observed.root.rawMakeSymlink(at: "sub/way-out", pointingAt: observed.root.path)

        let top = try observed.fileSystem.listDirectory(try rootLocation(of: observed), finalSymlink: .refuse)
        let sub = try observed.fileSystem.listDirectory(try observed.location("sub"), finalSymlink: .refuse)

        #expect(
            top == [
                Entry(entryType: .regularFile, name: Array("a.txt".utf8)),
                Entry(entryType: .directory, name: Array("sub".utf8)),
            ])
        // The symlink is listed as a symlink and never followed, although it points back at the tree.
        #expect(
            sub == [
                Entry(entryType: .regularFile, name: Array("b.txt".utf8)),
                Entry(entryType: .symlink, name: Array("way-out".utf8)),
            ])
    }

    /// More entries than the first array holds, so the listing is asked for a second time with the capacity the
    /// core named.
    @Test(.disabled("stage 15 directories-in-c"))
    func aListingLargerThanItsFirstBuffersIsComplete() throws {
        let observed = try observedFileSystem()
        var expected: [String] = []
        for number in 100..<400 {
            let name = "entry-with-a-reasonably-long-name-\(number).txt"
            try observed.root.rawWrite("", to: name, mode: 0o644)
            expected.append(name)
        }

        let entries = try observed.fileSystem.listDirectory(try rootLocation(of: observed), finalSymlink: .refuse)

        #expect(entries.map(\.nameText) == expected)
    }

    @Test(.disabled("stage 15 directories-in-c"))
    func listOfAnEmptyDirectoryIsEmpty() throws {
        let observed = try observedFileSystem()

        #expect(try observed.fileSystem.listDirectory(try rootLocation(of: observed), finalSymlink: .refuse) == [])
    }

    // MARK: - Creating directories

    @Test(.disabled("stage 15 directories-in-c"))
    func createDirectoriesMakesEveryMissingDirectoryWithTheMode() throws {
        let observed = try observedFileSystem()

        try observed.fileSystem.createDirectories(try observed.location("a/b/c"), permissions: .exactly(mode: 0o750))

        #expect(try observed.root.rawMode(of: "a") == 0o750)
        #expect(try observed.root.rawMode(of: "a/b") == 0o750)
        #expect(try observed.root.rawMode(of: "a/b/c") == 0o750)
    }

    @Test(.disabled("stage 15 directories-in-c"))
    func createDirectoriesLeavesWhatExistsAsItIs() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("existing", mode: 0o700)

        try observed.fileSystem.createDirectories(
            try observed.location("existing"), permissions: .exactly(mode: 0o755))
        try observed.fileSystem.createDirectories(
            try observed.location("existing/new"), permissions: .exactly(mode: 0o755))

        #expect(try observed.root.rawMode(of: "existing") == 0o700)
        #expect(try observed.root.rawMode(of: "existing/new") == 0o755)
    }

    @Test(.disabled("stage 15 directories-in-c"))
    func createDirectoriesWhereAFileIsIsNotDirectory() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "file", mode: 0o644)
        let location = try observed.location("file")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.createDirectories(location, permissions: .exactly(mode: 0o755))
        }

        #expect(error?.kind == .notDirectory)
    }

    // MARK: - The core's buffer protocol

    /// The face grows its buffers itself, so the core is called directly here: a buffer that is too small is
    /// refused, the call reports the length it needs, and nothing is cut short into the buffer.
    @Test
    func aBufferTooSmallReportsTheLengthItNeeds() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("eleven byte", to: "file", mode: 0o644)
        let path = Array(observed.root.absolutePath(of: "file").utf8)
        var coreOptions = Self.wholeFileRefusingSymlink.core
        var coreError = bc_file_system_error()
        var length = 0
        var buffer = [UInt8](repeating: 0x3F, count: 10)

        let outcome = observed.fileSystem.engine.withHandle { handle in
            path.withUnsafeBufferPointer { pathBuffer in
                var location = bc_file_system_location_at(pathBuffer.baseAddress, pathBuffer.count)
                return bc_file_system_read_file(
                    handle, &location, &coreOptions, &buffer, buffer.count, &length, &coreError)
            }
        }

        #expect(outcome == Int32(bc_file_system_error_kind_buffer_too_small.rawValue))
        #expect(coreError.kind == Int32(bc_file_system_error_kind_buffer_too_small.rawValue))
        #expect(length == 11)
        #expect(buffer == [UInt8](repeating: 0x3F, count: 10), "nothing may be cut short into the buffer")
    }

    // MARK: - The two sides of the core

    /// The Swift error kinds are the core's numbers. This is what holds the two lists together.
    @Test func everyErrorKindIsTheCoresValue() {
        #expect(ErrorKind.notFound.rawValue == Int32(bc_file_system_error_kind_not_found.rawValue))
        #expect(ErrorKind.notRegularFile.rawValue == Int32(bc_file_system_error_kind_not_regular_file.rawValue))
        #expect(ErrorKind.notDirectory.rawValue == Int32(bc_file_system_error_kind_not_directory.rawValue))
        #expect(ErrorKind.permissionDenied.rawValue == Int32(bc_file_system_error_kind_permission_denied.rawValue))
        #expect(ErrorKind.alreadyExists.rawValue == Int32(bc_file_system_error_kind_already_exists.rawValue))
        #expect(ErrorKind.directoryNotEmpty.rawValue == Int32(bc_file_system_error_kind_directory_not_empty.rawValue))
        #expect(ErrorKind.symlinkRefused.rawValue == Int32(bc_file_system_error_kind_symlink_refused.rawValue))
        #expect(ErrorKind.tooLarge.rawValue == Int32(bc_file_system_error_kind_too_large.rawValue))
        #expect(ErrorKind.crossDevice.rawValue == Int32(bc_file_system_error_kind_cross_device.rawValue))
        #expect(ErrorKind.noSpace.rawValue == Int32(bc_file_system_error_kind_no_space.rawValue))
        #expect(ErrorKind.resourceExhausted.rawValue == Int32(bc_file_system_error_kind_resource_exhausted.rawValue))
        #expect(ErrorKind.unsupported.rawValue == Int32(bc_file_system_error_kind_unsupported.rawValue))
        #expect(ErrorKind.invalidArgument.rawValue == Int32(bc_file_system_error_kind_invalid_argument.rawValue))
        #expect(ErrorKind.ioFailed.rawValue == Int32(bc_file_system_error_kind_io_failed.rawValue))
        #expect(ErrorKind.bufferTooSmall.rawValue == Int32(bc_file_system_error_kind_buffer_too_small.rawValue))
    }
}
