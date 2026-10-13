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
/// A case of a permission failure returns early where the process reads what its permission bits forbid (the
/// superuser), because no such failure can happen there.
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

    @Test
    func writeThenReadReturnsTheSameBytes() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("payload.bin")
        let bytes: [UInt8] = [0, 1, 2, 254, 255]

        let achieved = try observed.fileSystem.writeFile(location, bytes: bytes, options: Self.durableExactly0640)

        #expect(achieved == .durable)
        #expect(try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink) == bytes)
        #expect(try observed.root.rawMode(of: "payload.bin") == 0o640)
    }

    @Test
    func anEmptyFileRoundTrips() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("empty")

        _ = try observed.fileSystem.writeFile(location, bytes: [], options: Self.writtenExactly0600)

        #expect(try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink) == [])
    }

    @Test
    func aFileLargerThanAnyInternalBufferRoundTrips() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("large.bin")
        let bytes = [UInt8](repeating: 0x5A, count: 3 * 1024 * 1024)

        _ = try observed.fileSystem.writeFile(location, bytes: bytes, options: Self.writtenExactly0644)

        #expect(try observed.fileSystem.readFile(location, options: Self.wholeFileRefusingSymlink) == bytes)
    }

    @Test
    func writeReplacesTheContentAndLeavesNoTemporaryFile() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("config")
        try observed.root.rawWrite("old content, and longer than the new", to: "config", mode: 0o644)

        _ = try observed.fileSystem.writeFile(location, bytes: Array("new".utf8), options: Self.writtenExactly0644)

        #expect(try observed.root.rawRead("config") == "new")
        #expect(try observed.root.rawNames() == ["config"])
    }

    /// A read that starts with a buffer too small (the face offers 1 MiB first) is asked again with the size the
    /// core reports.
    @Test
    func aReadGrowsItsBufferUntilTheFileFits() throws {
        let observed = try observedFileSystem()
        let text = String(repeating: "longer than the first buffer\n", count: 40 * 1024)
        try observed.root.rawWrite(text, to: "notes.txt", mode: 0o644)

        let bytes = try observed.fileSystem.readFile(
            try observed.location("notes.txt"), options: Self.wholeFileRefusingSymlink)

        #expect(String(decoding: bytes, as: UTF8.self) == text)
    }

    /// The face's buffer is never zero-filled, so an answer shorter than the buffer it was read into must hold
    /// the file's own bytes and nothing of what the buffer held before: a large read first leaves its bytes where
    /// the allocator may hand the next buffer out again, and the short file read next is answered as exactly its
    /// bytes, its count its own, under either limit.
    @Test
    func aFileShorterThanTheFirstOfferIsAnsweredAsOnlyItsOwnBytes() throws {
        let observed = try observedFileSystem()
        let large = String(repeating: "L", count: 900 * 1024)
        try observed.root.rawWrite(large, to: "large.bin", mode: 0o644)
        try observed.root.rawWrite("seven b", to: "short.txt", mode: 0o644)
        let atMostAMebibyte = ReadOptions(limit: .atMost(bytes: 1024 * 1024), finalSymlink: .refuse)

        let first = try observed.fileSystem.readFile(
            try observed.location("large.bin"), options: Self.wholeFileRefusingSymlink)
        let whole = try observed.fileSystem.readFile(
            try observed.location("short.txt"), options: Self.wholeFileRefusingSymlink)
        let bounded = try observed.fileSystem.readFile(try observed.location("short.txt"), options: atMostAMebibyte)

        #expect(first.count == large.utf8.count)
        #expect(whole == Array("seven b".utf8))
        #expect(bounded == Array("seven b".utf8))
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

    @Test
    func writeIntoADirectoryThatIsNotThereIsNotFound() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("missing/file")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.writeFile(location, bytes: [1], options: Self.writtenExactly0644)
        }

        #expect(error?.kind == .notFound)
    }

    @Test
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

    @Test
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

    @Test
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

    @Test
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

    /// A directory with mode 0o300 lets a path-based open create files in it, and the engine creates the file
    /// beneath the destination's directory held as a descriptor: the resolution opens it for search only, as the
    /// system treats such a directory, never read-only, which the mode forbids, so the file lands, and what
    /// cannot be opened read-only afterwards for its sync lowers the answer.
    @Test
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

    @Test
    func aWrittenWriteReportsWritten() throws {
        let observed = try observedFileSystem()

        let achieved = try observed.fileSystem.writeFile(
            try observed.location("file"), bytes: [1], options: Self.writtenExactly0644)

        #expect(achieved == .written)
    }

    // MARK: - Permissions

    @Test
    func aReplacedFileKeepsItsModeUnderPreserveOr() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("config")
        try observed.root.rawWrite("old", to: "config", mode: 0o640)

        _ = try observed.fileSystem.writeFile(location, bytes: Array("new".utf8), options: Self.writtenPreserveOr0600)

        #expect(try observed.root.rawRead("config") == "new")
        #expect(try observed.root.rawMode(of: "config") == 0o640)
    }

    @Test
    func aNewFileGetsTheStatedModeUnderPreserveOr() throws {
        let observed = try observedFileSystem()
        let location = try observed.location("config")

        _ = try observed.fileSystem.writeFile(location, bytes: Array("new".utf8), options: Self.writtenPreserveOr0600)

        #expect(try observed.root.rawMode(of: "config") == 0o600)
    }

    @Test
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

    @Test
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

    /// More entries than the first array holds (1,024), so the listing is asked for a second time with the
    /// capacity the core named.
    @Test
    func aListingLargerThanItsFirstBuffersIsComplete() throws {
        let observed = try observedFileSystem()
        var expected: [String] = []
        for number in 1000..<2100 {
            let name = "entry-with-a-reasonably-long-name-\(number).txt"
            try observed.root.rawWrite("", to: name, mode: 0o644)
            expected.append(name)
        }

        let entries = try observed.fileSystem.listDirectory(try rootLocation(of: observed), finalSymlink: .refuse)

        #expect(entries.map(\.nameText) == expected)
    }

    @Test
    func listOfAnEmptyDirectoryIsEmpty() throws {
        let observed = try observedFileSystem()

        #expect(try observed.fileSystem.listDirectory(try rootLocation(of: observed), finalSymlink: .refuse) == [])
    }

    // MARK: - Creating directories

    @Test
    func createDirectoriesMakesEveryMissingDirectoryWithTheMode() throws {
        let observed = try observedFileSystem()

        try observed.fileSystem.createDirectories(try observed.location("a/b/c"), permissions: .exactly(mode: 0o750))

        #expect(try observed.root.rawMode(of: "a") == 0o750)
        #expect(try observed.root.rawMode(of: "a/b") == 0o750)
        #expect(try observed.root.rawMode(of: "a/b/c") == 0o750)
    }

    @Test
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

    @Test
    func createDirectoriesWhereAFileIsIsNotDirectory() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "file", mode: 0o644)
        let location = try observed.location("file")

        let error = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.createDirectories(location, permissions: .exactly(mode: 0o755))
        }

        #expect(error?.kind == .notDirectory)
    }

    // MARK: - Removing, and a temporary directory

    @Test
    func removeTreeRemovesEverythingAndASecondCallFindsNothing() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("tree", mode: 0o755)
        try observed.root.rawMakeDirectory("tree/sub", mode: 0o755)
        try observed.root.rawWrite("content", to: "tree/sub/file", mode: 0o644)
        try observed.root.rawMakeSymlink(at: "tree/way-out", pointingAt: observed.root.path)
        let location = try observed.location("tree")

        let first = try observed.fileSystem.removeTree(location)
        let second = try observed.fileSystem.removeTree(location)

        #expect(first == true)
        #expect(second == false)
        #expect(observed.root.rawExists("tree") == false)
    }

    @Test
    func createTemporaryDirectoryTwiceYieldsTwoNamesWithTheMode() throws {
        let observed = try observedFileSystem()
        let parent = try rootLocation(of: observed)

        let first = try observed.fileSystem.createTemporaryDirectory(in: parent, permissions: .exactly(mode: 0o700))
        let second = try observed.fileSystem.createTemporaryDirectory(in: parent, permissions: .exactly(mode: 0o750))

        #expect(first != second)
        let firstName = String(decoding: first, as: UTF8.self)
        let secondName = String(decoding: second, as: UTF8.self)
        #expect(try observed.root.rawMode(of: firstName) == 0o700)
        #expect(try observed.root.rawMode(of: secondName) == 0o750)
        #expect(try observed.root.rawNames() == [firstName, secondName].sorted())
    }

    // MARK: - Moving and copying

    @Test
    func renameMovesAFileAndUnderRefuseLeavesAnExistingDestination() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("the new content", to: "staged", mode: 0o644)
        try observed.root.rawWrite("the old content", to: "current", mode: 0o644)
        let staged = try observed.location("staged")
        let current = try observed.location("current")

        let refused = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.rename(
                from: staged, to: current, existingDestination: .refuse, durability: .written)
        }
        let achieved = try observed.fileSystem.rename(
            from: staged, to: current, existingDestination: .replace, durability: .durable)

        #expect(refused?.kind == .alreadyExists)
        #expect(achieved == .durable)
        #expect(try observed.root.rawRead("current") == "the new content")
        #expect(try observed.root.rawNames() == ["current"])
    }

    @Test
    func copyWritesTheSourcesBytesWithTheStatedModeAndLeavesTheSource() throws {
        let observed = try observedFileSystem()
        let bytes = [UInt8](repeating: 0xA5, count: 3 * 1024 * 1024)
        let source = try observed.location("source")
        _ = try observed.fileSystem.writeFile(source, bytes: bytes, options: Self.writtenExactly0644)

        let achieved = try observed.fileSystem.copyFile(
            from: source, to: try observed.location("copy"), options: Self.writtenExactly0600)

        #expect(achieved == .written)
        #expect(
            try observed.fileSystem.readFile(try observed.location("copy"), options: Self.wholeFileRefusingSymlink)
                == bytes)
        #expect(try observed.root.rawMode(of: "copy") == 0o600)
        #expect(try observed.root.rawMode(of: "source") == 0o644)
        #expect(try observed.root.rawNames() == ["copy", "source"])
    }

    // MARK: - Locks, links and the condemned sweep

    /// The lock is held by the open-file description the handle owns, so a second engine in the same process
    /// (a second observed file system over the same directory) is refused while the first holds it and takes it
    /// once the first handle is gone.
    @Test
    func tryLockAcquiresAndASecondEngineInTheSameProcessIsRefusedUntilItIsReleased() throws {
        let observed = try observedFileSystem()
        let second = try FileSystem.observed(root: observed.root)
        try observed.root.rawWrite("", to: "file.lock", mode: 0o644)
        let location = try observed.location("file.lock")

        var whileHeld: Lock?
        do {
            let first = try observed.fileSystem.tryLock(location)
            #expect(first != nil)
            whileHeld = try second.fileSystem.tryLock(location)
        }
        let afterRelease = try second.fileSystem.tryLock(location)

        #expect(whileHeld == nil)
        #expect(afterRelease != nil)
    }

    @Test
    func tryLockOnASymlinkIsRefusedAndOnNothingIsNotFound() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("", to: "target", mode: 0o644)
        try observed.root.rawMakeSymlink(at: "link", pointingAt: "target")
        let link = try observed.location("link")
        let absent = try observed.location("absent.lock")

        let throughLink = errorThrown { () throws(FileSystemError) in try observed.fileSystem.tryLock(link) }
        let ofNothing = errorThrown { () throws(FileSystemError) in try observed.fileSystem.tryLock(absent) }

        #expect(throughLink?.kind == .symlinkRefused)
        #expect(ofNothing?.kind == .notFound)
        #expect(observed.root.rawExists("absent.lock") == false)
    }

    /// The target goes to the system as spelled and comes back the same; an existing entry refuses the link.
    @Test
    func createSymlinkStoresTheTargetUnchangedAndReadSymlinkReadsItBack() throws {
        let observed = try observedFileSystem()
        let target = Array("../elsewhere/./target.txt".utf8)
        let link = try observed.location("link")

        try observed.fileSystem.createSymlink(target: target, at: link)
        let text = try observed.fileSystem.readSymlink(link)
        let again = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.createSymlink(target: target, at: link)
        }

        #expect(text == target)
        #expect(again?.kind == .alreadyExists)
        #expect(again?.step == .symlinkat)
    }

    /// A dangling link reads like any other; a file is no link, which the system answers with EINVAL and the
    /// face passes on as `ioFailed`.
    @Test
    func readSymlinkReadsADanglingLinkAndRefusesAFileWithEinval() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeSymlink(at: "dangling", pointingAt: "nowhere/at/all")
        try observed.root.rawWrite("content", to: "file", mode: 0o644)
        let file = try observed.location("file")

        let text = try observed.fileSystem.readSymlink(try observed.location("dangling"))
        let ofFile = errorThrown { () throws(FileSystemError) in try observed.fileSystem.readSymlink(file) }

        #expect(String(decoding: text, as: UTF8.self) == "nowhere/at/all")
        #expect(ofFile?.kind == .ioFailed)
        #expect(ofFile?.systemCode == EINVAL)
    }

    /// Condemn twice under one name yields two names; the sweep clears both and the staged leftover, and leaves
    /// the neighbour.
    @Test
    func condemnTwiceYieldsTwoNamesAndRemoveCondemnedClearsThemAndAStagedLeftover() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("tree", mode: 0o755)
        try observed.root.rawWrite("first", to: "tree/file", mode: 0o644)
        let tree = try observed.location("tree")
        let first = try observed.fileSystem.condemn(tree)
        try observed.root.rawMakeDirectory("tree", mode: 0o755)
        try observed.root.rawWrite("second", to: "tree/file", mode: 0o644)
        let second = try observed.fileSystem.condemn(tree)
        try observed.root.rawWrite("leftover", to: ".ledger.bc-tmp-4242-7", mode: 0o600)
        try observed.root.rawWrite("stays", to: "neighbour", mode: 0o644)
        let firstName = String(decoding: first, as: UTF8.self)
        let secondName = String(decoding: second, as: UTF8.self)
        #expect(firstName == ".bc-condemned-tree")
        #expect(secondName.hasPrefix(".bc-condemned-tree-"))
        #expect(try observed.root.rawNames() == [firstName, secondName, ".ledger.bc-tmp-4242-7", "neighbour"].sorted())

        try observed.fileSystem.removeCondemned(in: try rootLocation(of: observed))

        #expect(try observed.root.rawNames() == ["neighbour"])
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
