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

/// The Swift binding suite: the face's contract with the C header, apart from what the operations do to the file
/// system (the contract suite's job). Each operation reaches the core once and answers from the file system,
/// the mapping of every enum is total in both directions, each value type makes one
/// round trip across the ABI, every struct has the layout the core pins, the error struct is converted and
/// rendered, and the face's own compositions do what the design doc says. Green from this stage: it tests the
/// face, not the engine.
struct BindingTests {
    // The options every case states, built once here so a field added later fails in one place.
    static let wholeFileRefusingSymlink = ReadOptions(limit: .wholeFile, finalSymlink: .refuse)
    static let writtenExactly0644 = WriteOptions(
        durability: .written, permissions: .exactly(mode: 0o644), existingDestination: .replace, finalSymlink: .refuse)
    static let durableExactly0600 = WriteOptions(
        durability: .durable, permissions: .exactly(mode: 0o600), existingDestination: .replace, finalSymlink: .refuse)

    private func observedFileSystem() throws -> ObservedFileSystem {
        try FileSystem.observed(root: TemporaryDirectory.create())
    }

    // MARK: - Every operation reaches C

    /// The one operation that is real: the handle comes back with the text it was opened with.
    @Test func openDirectoryReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("folder", mode: 0o755)
        let directory = try observed.fileSystem.openDirectory(
            try observed.location("folder"), finalSymlink: .refuse)
        #expect(directory.openedAsText == Array(observed.root.absolutePath(of: "folder").utf8))
    }

    /// The status operation is real: the answer comes back through the face's conversion of the C struct.
    @Test func statusReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("seven b", to: "file", mode: 0o640)

        let status = try #require(
            try observed.fileSystem.status(try observed.location("file"), finalSymlink: .refuse))

        #expect(status.entryType == .regularFile)
        #expect(status.size == 7)
        #expect(status.mode == 0o640)
    }

    /// The read operation is real: the bytes come back through the face's buffer protocol.
    @Test func readFileReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "file", mode: 0o644)

        let bytes = try observed.fileSystem.readFile(
            try observed.location("file"), options: Self.wholeFileRefusingSymlink)

        #expect(String(decoding: bytes, as: UTF8.self) == "content")
    }

    /// The write operation is real: the bytes and the mode land, and the durability achieved comes back.
    @Test func writeFileReachesC() throws {
        let observed = try observedFileSystem()

        let achieved = try observed.fileSystem.writeFile(
            try observed.location("file"), bytes: Array("content".utf8), options: Self.writtenExactly0644)

        #expect(achieved == .written)
        #expect(try observed.root.rawRead("file") == "content")
        #expect(try observed.root.rawMode(of: "file") == 0o644)
    }

    /// The append operation is real: a file it creates gets the mode, and the bytes are in it.
    @Test func appendReachesC() throws {
        let observed = try observedFileSystem()

        let achieved = try observed.fileSystem.append(
            try observed.location("log"), bytes: Array("entry\n".utf8), durability: .written,
            permissions: .exactly(mode: 0o644))

        #expect(achieved == .written)
        #expect(try observed.root.rawRead("log") == "entry\n")
        #expect(try observed.root.rawMode(of: "log") == 0o644)
    }

    /// The sync operation is real: a file this platform can sync answers `durable`.
    @Test func syncReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "file", mode: 0o644)

        let achieved = try observed.fileSystem.sync(try observed.location("file"))

        #expect(achieved == .durable)
    }

    /// The directory operations are real: each one reaches the core and its answer comes back through the face.
    @Test func createDirectoryReachesC() throws {
        let observed = try observedFileSystem()

        try observed.fileSystem.createDirectory(try observed.location("folder"), permissions: .exactly(mode: 0o755))

        #expect(try observed.root.rawMode(of: "folder") == 0o755)
    }

    @Test func createDirectoriesReachesC() throws {
        let observed = try observedFileSystem()

        try observed.fileSystem.createDirectories(try observed.location("a/b/c"), permissions: .exactly(mode: 0o755))

        #expect(try observed.root.rawMode(of: "a/b/c") == 0o755)
    }

    @Test func listDirectoryReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("folder", mode: 0o755)
        try observed.root.rawWrite("content", to: "folder/file", mode: 0o644)

        let entries = try observed.fileSystem.listDirectory(try observed.location("folder"), finalSymlink: .refuse)

        #expect(entries == [Entry(entryType: .regularFile, name: Array("file".utf8))])
    }

    @Test func removeFileReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "file", mode: 0o644)

        let removed = try observed.fileSystem.removeFile(try observed.location("file"))

        #expect(removed == true)
        #expect(observed.root.rawExists("file") == false)
    }

    @Test func removeDirectoryReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("folder", mode: 0o755)

        let removed = try observed.fileSystem.removeDirectory(try observed.location("folder"))

        #expect(removed == true)
        #expect(observed.root.rawExists("folder") == false)
    }

    @Test func removeTreeReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("tree", mode: 0o755)
        try observed.root.rawWrite("content", to: "tree/file", mode: 0o644)

        let removed = try observed.fileSystem.removeTree(try observed.location("tree"))

        #expect(removed == true)
        #expect(observed.root.rawExists("tree") == false)
    }

    @Test func createTemporaryDirectoryReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("parent", mode: 0o755)

        let name = try observed.fileSystem.createTemporaryDirectory(
            in: try observed.location("parent"), permissions: .exactly(mode: 0o700))

        #expect(name.isEmpty == false)
        #expect(try observed.root.rawMode(of: "parent/" + String(decoding: name, as: UTF8.self)) == 0o700)
    }

    /// A rename is real, and the error of a two-location operation names the end it belongs to: the face reads
    /// the core's `failed_location` and composes the path from the source or the destination accordingly.
    @Test func renameReachesCAndItsErrorNamesTheEndThatFailed() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "from", mode: 0o644)

        let achieved = try observed.fileSystem.rename(
            from: try observed.location("from"), to: try observed.location("to"), existingDestination: .replace,
            durability: .written)

        #expect(achieved == .written)
        #expect(try observed.root.rawRead("to") == "content")
        #expect(observed.root.rawExists("from") == false)
        let ofNothing = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.rename(
                from: try observed.location("from"), to: try observed.location("to"), existingDestination: .replace,
                durability: .written)
        }
        let intoNothing = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.rename(
                from: try observed.location("to"), to: try observed.location("missing/to"),
                existingDestination: .replace, durability: .written)
        }
        #expect(ofNothing?.kind == .notFound)
        #expect(ofNothing?.operation == .rename)
        #expect(ofNothing?.failedLocation == .first)
        #expect(ofNothing?.path == observed.root.absolutePath(of: "from"))
        #expect(intoNothing?.kind == .notFound)
        #expect(intoNothing?.failedLocation == .second)
        #expect(intoNothing?.path == observed.root.absolutePath(of: "missing/to"))
    }

    @Test func copyFileReachesCAndItsErrorNamesTheEndThatFailed() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "from", mode: 0o644)

        let achieved = try observed.fileSystem.copyFile(
            from: try observed.location("from"), to: try observed.location("to"), options: Self.writtenExactly0644)

        #expect(achieved == .written)
        #expect(try observed.root.rawRead("to") == "content")
        #expect(try observed.root.rawRead("from") == "content")
        let ofNothing = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.copyFile(
                from: try observed.location("absent"), to: try observed.location("to"),
                options: Self.writtenExactly0644)
        }
        let intoNothing = errorThrown { () throws(FileSystemError) in
            try observed.fileSystem.copyFile(
                from: try observed.location("from"), to: try observed.location("missing/to"),
                options: Self.writtenExactly0644)
        }
        #expect(ofNothing?.kind == .notFound)
        #expect(ofNothing?.operation == .copyFile)
        #expect(ofNothing?.failedLocation == .first)
        #expect(ofNothing?.path == observed.root.absolutePath(of: "absent"))
        #expect(intoNothing?.failedLocation == .second)
        #expect(intoNothing?.path == observed.root.absolutePath(of: "missing/to"))
    }

    @Test func setPermissionsReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("content", to: "file", mode: 0o644)

        try observed.fileSystem.setPermissions(try observed.location("file"), mode: 0o600, finalSymlink: .refuse)

        #expect(try observed.root.rawMode(of: "file") == 0o600)
    }

    /// The link operations are real: the target crosses as bytes, lands, and comes back through the grow loop.
    @Test func createSymlinkReachesC() throws {
        let observed = try observedFileSystem()

        try observed.fileSystem.createSymlink(target: Array("target".utf8), at: try observed.location("link"))

        #expect(try observed.fileSystem.readSymlink(try observed.location("link")) == Array("target".utf8))
    }

    @Test func readSymlinkReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeSymlink(at: "link", pointingAt: "target")

        let text = try observed.fileSystem.readSymlink(try observed.location("link"))

        #expect(text == Array("target".utf8))
    }

    /// The lock is real: the handle comes back under `acquired`, a second ask answers nil for `heldElsewhere`,
    /// and the handle going away lets the lock go.
    @Test func tryLockReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawWrite("", to: "file.lock", mode: 0o644)
        let location = try observed.location("file.lock")

        var refusedWhileHeld: Lock?
        do {
            let held = try observed.fileSystem.tryLock(location)
            #expect(held != nil)
            refusedWhileHeld = try observed.fileSystem.tryLock(location)
        }
        let again = try observed.fileSystem.tryLock(location)

        #expect(refusedWhileHeld == nil)
        #expect(again != nil)
    }

    @Test func condemnReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("tree", mode: 0o755)

        let name = try observed.fileSystem.condemn(try observed.location("tree"))

        #expect(String(decoding: name, as: UTF8.self) == ".bc-condemned-tree")
        #expect(observed.root.rawExists(".bc-condemned-tree"))
    }

    @Test func removeCondemnedReachesC() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("parent", mode: 0o755)
        try observed.root.rawMakeDirectory("parent/.bc-condemned-tree", mode: 0o755)
        try observed.root.rawWrite("kept", to: "parent/kept", mode: 0o644)

        try observed.fileSystem.removeCondemned(in: try observed.location("parent"))

        #expect(try observed.root.rawNames() == ["parent"])
        #expect(observed.root.rawExists("parent/.bc-condemned-tree") == false)
        #expect(observed.root.rawExists("parent/kept"))
    }

    /// The observer's three calls reach C: each succeeds on an observed engine, and a fresh engine has counted
    /// nothing.
    @Test func addFaultClearFaultsAndStepCountReachC() throws {
        let observed = try observedFileSystem()
        let fault = Fault(operation: .writeFile, step: .write, occurrence: 1, action: .fail, systemCode: ENOSPC)

        try observed.addFault(fault)
        try observed.clearFaults()

        #expect(try observed.stepCount(operation: .writeFile, step: .write) == 0)
    }

    /// The observer's own bookkeeping, which is real this stage.
    @Test func aFaultWithOccurrenceZeroIsRefusedAndTheSlotHoldsSixtyFour() throws {
        let observed = try observedFileSystem()

        let refused = errorThrown { () throws(FileSystemError) in
            try observed.addFault(
                Fault(operation: .writeFile, step: .write, occurrence: 0, action: .fail, systemCode: ENOSPC))
        }
        for occurrence in UInt32(1)...64 {
            try observed.addFault(
                Fault(operation: .writeFile, step: .write, occurrence: occurrence, action: .fail, systemCode: ENOSPC))
        }
        let oneMore = errorThrown { () throws(FileSystemError) in
            try observed.addFault(
                Fault(operation: .writeFile, step: .write, occurrence: 65, action: .fail, systemCode: ENOSPC))
        }

        #expect(refused?.kind == .invalidArgument)
        #expect(refused?.operation == .observedFaultAdd)
        #expect(oneMore?.kind == .resourceExhausted)
    }

    // MARK: - The face's compositions

    /// Absence becomes nil; every other error passes through as received.
    @Test func readFileIfPresentPassesAnErrorOtherThanNotFoundThrough() throws {
        let observed = try observedFileSystem()
        try observed.root.rawMakeDirectory("folder", mode: 0o755)
        let folder = try observed.location("folder")

        let absent = try observed.fileSystem.readFileIfPresent(
            try observed.location("absent"), options: Self.wholeFileRefusingSymlink)
        var passedThrough: FileSystemError?
        do {
            _ = try observed.fileSystem.readFileIfPresent(folder, options: Self.wholeFileRefusingSymlink)
            Issue.record("expected not_regular_file to pass through")
        } catch {
            passedThrough = error
        }

        #expect(absent == nil)
        #expect(passedThrough?.kind == .notRegularFile)
        #expect(passedThrough?.operation == .readFile)
        #expect(passedThrough?.path == observed.root.absolutePath(of: "folder"))
    }

    @Test func linesOfSplitsOnNewlineOnlyAndLeavesEveryOtherByteAlone() {
        let buffer = Array("one\ntwo\r\nthree".utf8)

        #expect(lines(of: buffer).map { Array($0) } == [Array("one".utf8), Array("two\r".utf8), Array("three".utf8)])
    }

    @Test func linesOfATrailingNewlineAddsNoEmptyLine() {
        let buffer = Array("one\ntwo\n".utf8)

        #expect(lines(of: buffer).map { Array($0) } == [Array("one".utf8), Array("two".utf8)])
    }

    @Test func linesOfAnEmptyLineInTheMiddleIsALine() {
        let buffer = Array("one\n\nthree".utf8)

        #expect(lines(of: buffer).map { Array($0) } == [Array("one".utf8), [], Array("three".utf8)])
    }

    @Test func linesOfAnEmptyBufferHasNoLines() {
        #expect(lines(of: []).isEmpty)
    }

    // MARK: - The error mapping is total

    @Test func everyErrorKindHasItsCValue() {
        let pairs: [(bc_file_system_error_kind, ErrorKind)] = [
            (bc_file_system_error_kind_not_found, .notFound),
            (bc_file_system_error_kind_not_regular_file, .notRegularFile),
            (bc_file_system_error_kind_not_directory, .notDirectory),
            (bc_file_system_error_kind_permission_denied, .permissionDenied),
            (bc_file_system_error_kind_already_exists, .alreadyExists),
            (bc_file_system_error_kind_directory_not_empty, .directoryNotEmpty),
            (bc_file_system_error_kind_symlink_refused, .symlinkRefused),
            (bc_file_system_error_kind_too_large, .tooLarge),
            (bc_file_system_error_kind_cross_device, .crossDevice),
            (bc_file_system_error_kind_no_space, .noSpace),
            (bc_file_system_error_kind_resource_exhausted, .resourceExhausted),
            (bc_file_system_error_kind_unsupported, .unsupported),
            (bc_file_system_error_kind_invalid_argument, .invalidArgument),
            (bc_file_system_error_kind_io_failed, .ioFailed),
            (bc_file_system_error_kind_buffer_too_small, .bufferTooSmall),
        ]
        for (core, kind) in pairs {
            #expect(ErrorKind(rawValue: Int32(core.rawValue)) == kind)
        }
        // The list above and the enum are both the whole header: the last value pins the count.
        #expect(ErrorKind.allCases.count == pairs.count)
        #expect(ErrorKind.allCases.count == Int(bc_file_system_error_kind_buffer_too_small.rawValue))
        #expect(ErrorKind(rawValue: 0) == nil)
        #expect(ErrorKind(rawValue: 16) == nil)
    }

    @Test func everyOperationHasItsCValue() {
        let pairs: [(bc_file_system_operation, Operation)] = [
            (bc_file_system_operation_engine_create_live, .engineCreateLive),
            (bc_file_system_operation_engine_create_observed_for_tests, .engineCreateObservedForTests),
            (bc_file_system_operation_open_directory, .openDirectory),
            (bc_file_system_operation_directory_opened_as, .directoryOpenedAs),
            (bc_file_system_operation_observed_fault_add, .observedFaultAdd),
            (bc_file_system_operation_observed_faults_clear, .observedFaultsClear),
            (bc_file_system_operation_observed_step_count, .observedStepCount),
            (bc_file_system_operation_status, .status),
            (bc_file_system_operation_read_file, .readFile),
            (bc_file_system_operation_write_file, .writeFile),
            (bc_file_system_operation_append, .append),
            (bc_file_system_operation_sync, .sync),
            (bc_file_system_operation_create_directory, .createDirectory),
            (bc_file_system_operation_create_directories, .createDirectories),
            (bc_file_system_operation_list_directory, .listDirectory),
            (bc_file_system_operation_remove_file, .removeFile),
            (bc_file_system_operation_remove_directory, .removeDirectory),
            (bc_file_system_operation_remove_tree, .removeTree),
            (bc_file_system_operation_create_temporary_directory, .createTemporaryDirectory),
            (bc_file_system_operation_rename, .rename),
            (bc_file_system_operation_copy_file, .copyFile),
            (bc_file_system_operation_set_permissions, .setPermissions),
            (bc_file_system_operation_create_symlink, .createSymlink),
            (bc_file_system_operation_read_symlink, .readSymlink),
            (bc_file_system_operation_try_lock, .tryLock),
            (bc_file_system_operation_condemn, .condemn),
            (bc_file_system_operation_remove_condemned, .removeCondemned),
            (bc_file_system_operation_relative_path_from, .relativePathFrom),
            (bc_file_system_operation_absolute_path_from, .absolutePathFrom),
        ]
        for (core, operation) in pairs {
            #expect(Operation(rawValue: Int32(core.rawValue)) == operation)
        }
        #expect(Operation.allCases.count == pairs.count)
        #expect(Operation.allCases.count == Int(bc_file_system_operation_absolute_path_from.rawValue))
        #expect(Operation(rawValue: 0) == nil)
        #expect(Operation(rawValue: 30) == nil)
    }

    @Test func everyStepHasItsCValue() {
        let pairs: [(bc_file_system_step, Step)] = [
            (bc_file_system_step_open, .open),
            (bc_file_system_step_openat, .openat),
            (bc_file_system_step_fstat, .fstat),
            (bc_file_system_step_fstatat, .fstatat),
            (bc_file_system_step_read, .read),
            (bc_file_system_step_pread, .pread),
            (bc_file_system_step_write, .write),
            (bc_file_system_step_fsync, .fsync),
            (bc_file_system_step_fcntl_fullfsync, .fcntlFullfsync),
            (bc_file_system_step_renameat, .renameat),
            (bc_file_system_step_rename_no_replace, .renameNoReplace),
            (bc_file_system_step_unlinkat, .unlinkat),
            (bc_file_system_step_mkdirat, .mkdirat),
            (bc_file_system_step_fdopendir, .fdopendir),
            (bc_file_system_step_readdir, .readdir),
            (bc_file_system_step_symlinkat, .symlinkat),
            (bc_file_system_step_readlinkat, .readlinkat),
            (bc_file_system_step_flock, .flock),
            (bc_file_system_step_fchmod, .fchmod),
            (bc_file_system_step_copy_range, .copyRange),
            (bc_file_system_step_clone_file, .cloneFile),
            (bc_file_system_step_close, .close),
            (bc_file_system_step_identity_check, .identityCheck),
            (bc_file_system_step_openat2, .openat2),
            (bc_file_system_step_openat_nofollow_any, .openatNofollowAny),
            (bc_file_system_step_fchmodat, .fchmodat),
            (bc_file_system_step_faccessat, .faccessat),
        ]
        for (core, step) in pairs {
            #expect(Step(rawValue: Int32(core.rawValue)) == step)
        }
        #expect(Step.allCases.count == pairs.count)
        #expect(Step.allCases.count == Int(bc_file_system_step_faccessat.rawValue))
        #expect(Step(rawValue: 0) == nil)
        #expect(Step(rawValue: 28) == nil)
    }

    @Test func everySmallEnumIsPinnedToTheHeader() {
        #expect(FinalSymlink.refuse.rawValue == Int32(bc_file_system_final_symlink_refuse.rawValue))
        #expect(FinalSymlink.follow.rawValue == Int32(bc_file_system_final_symlink_follow.rawValue))
        #expect(ExistingDestination.replace.rawValue == Int32(bc_file_system_existing_destination_replace.rawValue))
        #expect(ExistingDestination.refuse.rawValue == Int32(bc_file_system_existing_destination_refuse.rawValue))
        #expect(Durability.written.rawValue == Int32(bc_file_system_durability_written.rawValue))
        #expect(Durability.durable.rawValue == Int32(bc_file_system_durability_durable.rawValue))
        #expect(EntryType.regularFile.rawValue == Int32(bc_file_system_entry_type_regular_file.rawValue))
        #expect(EntryType.directory.rawValue == Int32(bc_file_system_entry_type_directory.rawValue))
        #expect(EntryType.symlink.rawValue == Int32(bc_file_system_entry_type_symlink.rawValue))
        #expect(EntryType.other.rawValue == Int32(bc_file_system_entry_type_other.rawValue))
        #expect(LockOutcome.acquired.rawValue == Int32(bc_file_system_lock_outcome_acquired.rawValue))
        #expect(LockOutcome.heldElsewhere.rawValue == Int32(bc_file_system_lock_outcome_held_elsewhere.rawValue))
        #expect(FaultAction.fail.rawValue == Int32(bc_file_system_fault_action_fail.rawValue))
        #expect(FaultAction.stop.rawValue == Int32(bc_file_system_fault_action_stop.rawValue))
        // The header numbers the two locations in a comment at the field, not in an enum: 1 and 2, 0 for none.
        #expect(FailedLocation.first.rawValue == 1)
        #expect(FailedLocation.second.rawValue == 2)
        #expect(FailedLocation(rawValue: 0) == nil)
        #expect(FailedLocation(rawValue: 3) == nil)
        #expect(FailedLocation.allCases.count == 2)
        #expect(FinalSymlink.allCases.count == 2)
        #expect(ExistingDestination.allCases.count == 2)
        #expect(Durability.allCases.count == 2)
        #expect(EntryType.allCases.count == 4)
        #expect(LockOutcome.allCases.count == 2)
        #expect(FaultAction.allCases.count == 2)
    }

    // MARK: - The error struct crosses once

    private func coreError(
        kind: Int32, operation: Int32, step: Int32, systemCode: Int32, name: String
    ) -> bc_file_system_error {
        var error = bc_file_system_error()
        error.kind = kind
        error.operation = operation
        error.step = step
        error.system_code = systemCode
        let bytes = Array(name.utf8)
        error.name_length = UInt32(bytes.count)
        withUnsafeMutableBytes(of: &error.name) { field in
            field.copyBytes(from: bytes.prefix(field.count))
        }
        return error
    }

    @Test func anErrorComposesItsPathFromTheLocationAndTheEntryName() {
        let core = coreError(
            kind: Int32(bc_file_system_error_kind_not_found.rawValue),
            operation: Int32(bc_file_system_operation_read_file.rawValue),
            step: Int32(bc_file_system_step_openat.rawValue), systemCode: ENOENT, name: "child")

        let error = FileSystemError(core: core, called: .readFile, locationText: Array("/a/b".utf8))

        #expect(error.kind == .notFound)
        #expect(error.operation == .readFile)
        #expect(error.step == .openat)
        #expect(error.systemCode == ENOENT)
        #expect(error.path == "/a/b/child")
        #expect(error.nameTruncated == false)
    }

    @Test func anErrorsPathIsTheLocationAloneWhenTheStepActedOnIt() {
        let core = coreError(
            kind: Int32(bc_file_system_error_kind_permission_denied.rawValue),
            operation: Int32(bc_file_system_operation_status.rawValue),
            step: Int32(bc_file_system_step_fstatat.rawValue), systemCode: EACCES, name: "")

        let error = FileSystemError(core: core, called: .status, locationText: Array("/a/b".utf8))

        #expect(error.path == "/a/b")
    }

    @Test func anErrorWithNoStepHasNoStep() {
        let core = coreError(
            kind: Int32(bc_file_system_error_kind_invalid_argument.rawValue),
            operation: Int32(bc_file_system_operation_write_file.rawValue), step: 0, systemCode: 0, name: "")

        let error = FileSystemError(core: core, called: .writeFile, locationText: Array("relative.txt".utf8))

        #expect(error.step == nil)
        #expect(error.message == "writeFile: relative.txt: invalidArgument")
    }

    /// The field that says which end of a two-location operation failed crosses as itself, and the two-location
    /// conversion composes the path from the end it names: the first, the second, or the second when it names
    /// neither.
    @Test func theFailedLocationCrossesAndChoosesThePathOfATwoLocationError() {
        var atTheSource = coreError(
            kind: Int32(bc_file_system_error_kind_not_found.rawValue),
            operation: Int32(bc_file_system_operation_rename.rawValue),
            step: Int32(bc_file_system_step_fstatat.rawValue), systemCode: ENOENT, name: "")
        atTheSource.failed_location = 1
        var atTheDestination = coreError(
            kind: Int32(bc_file_system_error_kind_already_exists.rawValue),
            operation: Int32(bc_file_system_operation_rename.rawValue),
            step: Int32(bc_file_system_step_rename_no_replace.rawValue), systemCode: EEXIST, name: "")
        atTheDestination.failed_location = 2
        let atNeither = coreError(
            kind: Int32(bc_file_system_error_kind_invalid_argument.rawValue),
            operation: Int32(bc_file_system_operation_rename.rawValue), step: 0, systemCode: 0, name: "")
        let from = Array("/from".utf8)
        let to = Array("/to".utf8)

        let source = FileSystemError(
            core: atTheSource, called: .rename, firstLocationText: from, secondLocationText: to)
        let destination = FileSystemError(
            core: atTheDestination, called: .rename, firstLocationText: from, secondLocationText: to)
        let neither = FileSystemError(core: atNeither, called: .rename, firstLocationText: from, secondLocationText: to)
        let oneLocation = FileSystemError(core: atNeither, called: .rename, locationText: Array("/one".utf8))

        #expect(source.failedLocation == .first)
        #expect(source.path == "/from")
        #expect(destination.failedLocation == .second)
        #expect(destination.path == "/to")
        #expect(neither.failedLocation == nil)
        #expect(neither.path == "/to")
        #expect(oneLocation.failedLocation == nil)
        #expect(oneLocation.path == "/one")
    }

    /// The core's name field is bounded; a longer name arrives cut short and flagged, never silently.
    @Test func aNameLongerThanTheFieldArrivesCutShortAndFlagged() {
        let longName = String(repeating: "n", count: 256)
        var core = coreError(
            kind: Int32(bc_file_system_error_kind_not_found.rawValue),
            operation: Int32(bc_file_system_operation_status.rawValue),
            step: Int32(bc_file_system_step_fstatat.rawValue), systemCode: ENAMETOOLONG, name: longName)
        core.name_truncated = 1

        let error = FileSystemError(core: core, called: .status, locationText: Array("/a".utf8))

        #expect(error.path == "/a/" + longName)
        #expect(error.nameTruncated)
    }

    /// A core and a face built together cannot disagree on an enum (the pins above), so the stand-ins are never
    /// reached in practice; they are what the face answers rather than trusting a foreign integer.
    @Test func aValueOutsideTheEnumGetsAStandIn() {
        let core = coreError(kind: 99, operation: 99, step: 99, systemCode: 0, name: "")

        let error = FileSystemError(core: core, called: .sync, locationText: Array("/a".utf8))

        #expect(error.kind == .ioFailed)
        #expect(error.operation == .sync)
        #expect(error.step == nil)
    }

    @Test func theMessageNamesTheOperationThePathTheKindAndTheErrno() {
        let error = FileSystemError(
            kind: .notFound, operation: .readFile, step: .openat, systemCode: ENOENT, path: "/etc/config.toml",
            nameTruncated: false, failedLocation: nil)
        let withoutLocation = FileSystemError(
            kind: .resourceExhausted, operation: .engineCreateLive, step: nil, systemCode: 0, path: "",
            nameTruncated: false, failedLocation: nil)

        #expect(error.message == "readFile: /etc/config.toml: notFound (errno \(ENOENT))")
        #expect(withoutLocation.message == "engineCreateLive: resourceExhausted")
    }

    // MARK: - One ABI round trip per value type

    @Test func readLimitCrossesAsItsKindAndItsBound() {
        let whole = ReadLimit.wholeFile.core
        let bounded = ReadLimit.atMost(bytes: 512).core

        #expect(whole.kind == Int32(bc_file_system_read_limit_kind_whole_file.rawValue))
        #expect(whole.at_most == 0)
        #expect(bounded.kind == Int32(bc_file_system_read_limit_kind_at_most.rawValue))
        #expect(bounded.at_most == 512)
    }

    @Test func permissionsCrossAsPolicyAndMode() {
        let exactly = Permissions.exactly(mode: 0o640).core
        let preserving = Permissions.preserveOr(mode: 0o600).core

        #expect(exactly.policy == Int32(bc_file_system_permissions_policy_exactly.rawValue))
        #expect(exactly.mode == 0o640)
        #expect(preserving.policy == Int32(bc_file_system_permissions_policy_preserve_or.rawValue))
        #expect(preserving.mode == 0o600)
    }

    @Test func writeOptionsCrossAsFourFields() {
        let core = Self.durableExactly0600.core

        #expect(core.durability == Int32(bc_file_system_durability_durable.rawValue))
        #expect(core.permissions.policy == Int32(bc_file_system_permissions_policy_exactly.rawValue))
        #expect(core.permissions.mode == 0o600)
        #expect(core.existing_destination == Int32(bc_file_system_existing_destination_replace.rawValue))
        #expect(core.final_symlink == Int32(bc_file_system_final_symlink_refuse.rawValue))
    }

    @Test func readOptionsCrossAsLimitAndFinalSymlink() {
        let core = Self.wholeFileRefusingSymlink.core

        #expect(core.limit.kind == Int32(bc_file_system_read_limit_kind_whole_file.rawValue))
        #expect(core.final_symlink == Int32(bc_file_system_final_symlink_refuse.rawValue))
    }

    @Test func statusCrossesAndComesBackEqual() {
        let status = Status(
            entryType: .regularFile, size: 1234, mode: 0o640, device: 16_777_220, inode: 98_765_432_101, linkCount: 2)

        let core = status.core
        let back = Status(core: core)

        #expect(core.entry_type == Int32(bc_file_system_entry_type_regular_file.rawValue))
        #expect(core.size == 1234)
        #expect(core.link_count == 2)
        #expect(back == status)
    }

    @Test func aStatusWithAnEntryTypeOutsideTheEnumIsRefused() {
        var core = bc_file_system_status()
        core.entry_type = 0

        #expect(Status(core: core) == nil)
    }

    @Test func anEntryCrossesAsItsTypeAndItsNameBytes() {
        var core = bc_file_system_entry()
        core.entry_type = Int32(bc_file_system_entry_type_symlink.rawValue)
        let name = Array("notes.txt".utf8)
        core.name_length = UInt32(name.count)
        withUnsafeMutableBytes(of: &core.name) { field in
            field.copyBytes(from: name)
        }

        let entry = Entry(core: core)

        #expect(entry == Entry(entryType: .symlink, name: name))
        #expect(entry?.nameText == "notes.txt")
    }

    @Test func anEntryWithABadTypeOrAnImpossibleLengthIsRefused() {
        var badType = bc_file_system_entry()
        badType.entry_type = 7
        badType.name_length = 1
        var tooLong = bc_file_system_entry()
        tooLong.entry_type = Int32(bc_file_system_entry_type_regular_file.rawValue)
        tooLong.name_length = 257

        #expect(Entry(core: badType) == nil)
        #expect(Entry(core: tooLong) == nil)
    }

    @Test func aFaultCrossesAndComesBackEqual() {
        let fault = Fault(operation: .writeFile, step: .fcntlFullfsync, occurrence: 2, action: .fail, systemCode: EIO)

        let core = fault.core
        let back = Fault(core: core)

        #expect(core.operation == Int32(bc_file_system_operation_write_file.rawValue))
        #expect(core.step == Int32(bc_file_system_step_fcntl_fullfsync.rawValue))
        #expect(core.occurrence == 2)
        #expect(core.action == Int32(bc_file_system_fault_action_fail.rawValue))
        #expect(core.system_code == EIO)
        #expect(back == fault)
    }

    @Test func aFaultWithAFieldOutsideItsEnumIsRefused() {
        var core = Fault(operation: .writeFile, step: .write, occurrence: 1, action: .fail, systemCode: EIO).core
        core.action = 0

        #expect(Fault(core: core) == nil)
    }

    @Test func aLocationCrossesAsItsKindAndItsBorrowedBytes() throws {
        let location = Location.at(try AbsolutePath(validating: "/etc/config.toml"))

        let (kind, directory, bytes) = location.withCore { core in
            (
                core.pointee.kind, core.pointee.directory,
                Array(UnsafeBufferPointer(start: core.pointee.path, count: core.pointee.length))
            )
        }

        #expect(kind == Int32(bc_file_system_location_kind_at.rawValue))
        #expect(directory == nil)
        #expect(bytes == Array("/etc/config.toml".utf8))
        #expect(location.text == Array("/etc/config.toml".utf8))
    }

    // A `beneath` location's round trip through the core is the contract suite's openDirectory cases'.

    // MARK: - The layout every struct is pinned to

    /// The numbers the core asserts (the header-and-faces report's table): a face built against a header whose
    /// layout moved fails here.
    @Test func everyStructHasThePinnedLayout() {
        #expect(MemoryLayout<bc_file_system_error>.size == 284)
        #expect(MemoryLayout<bc_file_system_error>.offset(of: \.name) == 21)
        #expect(MemoryLayout<bc_file_system_error>.offset(of: \.failed_location) == 280)
        #expect(MemoryLayout<bc_file_system_location>.size == (MemoryLayout<OpaquePointer>.size == 8 ? 32 : 16))
        #expect(
            MemoryLayout<bc_file_system_location>.offset(of: \.length)
                == (MemoryLayout<OpaquePointer>.size == 8 ? 24 : 12))
        #expect(MemoryLayout<bc_file_system_read_limit>.size == 16)
        #expect(MemoryLayout<bc_file_system_read_limit>.offset(of: \.at_most) == 8)
        #expect(MemoryLayout<bc_file_system_permissions>.size == 8)
        #expect(MemoryLayout<bc_file_system_permissions>.offset(of: \.mode) == 4)
        #expect(MemoryLayout<bc_file_system_write_options>.size == 20)
        #expect(MemoryLayout<bc_file_system_write_options>.offset(of: \.final_symlink) == 16)
        #expect(MemoryLayout<bc_file_system_read_options>.size == 24)
        #expect(MemoryLayout<bc_file_system_read_options>.offset(of: \.final_symlink) == 16)
        #expect(MemoryLayout<bc_file_system_status>.size == 48)
        #expect(MemoryLayout<bc_file_system_status>.offset(of: \.link_count) == 40)
        #expect(MemoryLayout<bc_file_system_entry>.size == 264)
        #expect(MemoryLayout<bc_file_system_entry>.offset(of: \.name) == 8)
        #expect(MemoryLayout<bc_file_system_fault>.size == 20)
        #expect(MemoryLayout<bc_file_system_fault>.offset(of: \.system_code) == 16)
    }

    // MARK: - The header's own promises, at the C level

    /// An error struct full of leftovers, so a test can tell that a call cleared it.
    private func dirtyCoreError() -> bc_file_system_error {
        var error = bc_file_system_error()
        error.kind = 99
        error.operation = 99
        error.step = 99
        error.system_code = 99
        error.name_length = 99
        error.name_truncated = 1
        withUnsafeMutableBytes(of: &error.name) { field in
            _ = field.initializeMemory(as: UInt8.self, repeating: 0x78)
        }
        error.failed_location = 99
        return error
    }

    private func isAllZero(_ error: bc_file_system_error) -> Bool {
        withUnsafeBytes(of: error) { bytes in bytes.allSatisfy { $0 == 0 } }
    }

    @Test func aSuccessZeroesTheErrorStruct() throws {
        let observed = try observedFileSystem()
        var error = dirtyCoreError()

        let outcome = observed.fileSystem.engine.withHandle { handle in
            bc_file_system_observed_faults_clear(handle, &error)
        }

        #expect(outcome == 0)
        #expect(isAllZero(error))
    }

    /// Zero is never a valid enum input: a zeroed options struct is detectably unfilled, not a silent choice.
    @Test func aZeroEnumIsInvalidArgument() throws {
        let observed = try observedFileSystem()
        let path = Array(observed.root.absolutePath(of: "file").utf8)
        var zeroedOptions = bc_file_system_write_options()
        var achieved: Int32 = 0
        var error = bc_file_system_error()

        let outcome = observed.fileSystem.engine.withHandle { handle in
            path.withUnsafeBufferPointer { pathBuffer in
                var location = bc_file_system_location_at(pathBuffer.baseAddress, pathBuffer.count)
                return bc_file_system_write_file(handle, &location, nil, 0, &zeroedOptions, &achieved, &error)
            }
        }

        #expect(outcome == Int32(bc_file_system_error_kind_invalid_argument.rawValue))
        #expect(error.operation == Int32(bc_file_system_operation_write_file.rawValue))
    }

    @Test func aNullErrorPointerIsInvalidArgumentAndWritesNowhere() throws {
        let observed = try observedFileSystem()
        let path = Array(observed.root.absolutePath(of: "file").utf8)
        var present: Int32 = 7
        var status = bc_file_system_status()

        let outcome = observed.fileSystem.engine.withHandle { handle in
            path.withUnsafeBufferPointer { pathBuffer in
                var location = bc_file_system_location_at(pathBuffer.baseAddress, pathBuffer.count)
                return bc_file_system_status_of(
                    handle, &location, Int32(bc_file_system_final_symlink_refuse.rawValue), &present, &status, nil)
            }
        }

        #expect(outcome == Int32(bc_file_system_error_kind_invalid_argument.rawValue))
        #expect(present == 7)
    }

    /// A NULL byte pointer with a length of 0 is the stated way to pass no bytes, not a contract violation: the
    /// write gets past its checks and creates the empty file.
    @Test func writeOfNoBytesWithANullPointerIsNotAContractViolation() throws {
        let observed = try observedFileSystem()
        let path = Array(observed.root.absolutePath(of: "empty").utf8)
        var options = Self.writtenExactly0644.core
        var achieved: Int32 = 0
        var error = bc_file_system_error()

        let outcome = observed.fileSystem.engine.withHandle { handle in
            path.withUnsafeBufferPointer { pathBuffer in
                var location = bc_file_system_location_at(pathBuffer.baseAddress, pathBuffer.count)
                return bc_file_system_write_file(handle, &location, nil, 0, &options, &achieved, &error)
            }
        }

        #expect(outcome == 0)
        #expect(achieved == Int32(bc_file_system_durability_written.rawValue))
        #expect(try observed.root.rawRead("empty") == "")
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
}

/// The path values of the Swift face: what RelativePath and AbsolutePath accept and refuse, and how a Location
/// composes its text. The lexical rules live in the face until stage `path-module` moves them into C.
struct PathValueTests {
    @Test func aRelativePathAcceptsOneOrMoreComponents() throws {
        #expect(try RelativePath(validating: "notes.txt").bytes == Array("notes.txt".utf8))
        #expect(try RelativePath(validating: "notes/2026/april.md").text == "notes/2026/april.md")
    }

    @Test func aRelativePathRefusesWhatHasNoOneSpelling() {
        for spelling in ["", "/etc/passwd", "a//b", "a/b/", "..", "a/../b", "a/.."] {
            let refused = errorThrown { () throws(FileSystemError) in try RelativePath(validating: spelling) }
            #expect(refused?.kind == .invalidArgument, "\(spelling)")
            #expect(refused?.operation == .relativePathFrom, "\(spelling)")
            #expect(refused?.path == spelling, "\(spelling)")
        }
    }

    @Test func aRelativePathRefusesANulByte() {
        let refused = errorThrown { () throws(FileSystemError) in try RelativePath(validatingBytes: [0x61, 0, 0x62]) }

        #expect(refused?.kind == .invalidArgument)
    }

    @Test func anAbsolutePathAcceptsAPathStartingAtTheRootAndKeepsDotDot() throws {
        #expect(try AbsolutePath(validating: "/").bytes == Array("/".utf8))
        #expect(try AbsolutePath(validating: "/a/../b").text == "/a/../b")
    }

    @Test func anAbsolutePathRefusesRelativeTextAndANulByte() {
        for spelling in ["", "etc", "./etc", ".."] {
            let refused = errorThrown { () throws(FileSystemError) in try AbsolutePath(validating: spelling) }
            #expect(refused?.kind == .invalidArgument, "\(spelling)")
            #expect(refused?.operation == .absolutePathFrom, "\(spelling)")
        }
        let withNul = errorThrown { () throws(FileSystemError) in try AbsolutePath(validatingBytes: [0x2F, 0, 0x62]) }
        #expect(withNul?.kind == .invalidArgument)
    }

    @Test func aRefusalIsAnErrorLikeAnyOther() {
        let refused = errorThrown { () throws(FileSystemError) in try RelativePath(validating: "a/../b") }

        #expect(refused?.step == nil)
        #expect(refused?.systemCode == 0)
        #expect(refused?.nameTruncated == false)
        #expect(refused?.message == "relativePathFrom: a/../b: invalidArgument")
    }

    @Test func equalityIsAByteCompare() throws {
        #expect(try RelativePath(validating: "a/b") == RelativePath(validating: "a/b"))
        #expect(try RelativePath(validating: "a/b") != RelativePath(validating: "a/c"))
        #expect(try AbsolutePath(validating: "/a/b") != AbsolutePath(validating: "/a/b/"))
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
}
