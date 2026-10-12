import Foundation

// The tests' own way to the file system, and the one Swift file where a reach is legal (AGENTS.md, "Inject,
// never reach", legal site 3): a test may own a directory it created. This is the one place `swift test`
// reads where its root is, and the only Swift file that names `FileManager`. Each reach below carries the
// marker the source scan looks for, with its reason.
//
// A test sets a case up with the raw helpers here, and checks its outcome with them, so that what it asserts
// was measured without the library under test.

/// A setup step or a raw check that could not be done.
public struct FixtureFailure: Error, CustomStringConvertible, Sendable {
    public let description: String

    public init(description: String) {
        self.description = description
    }
}

/// A directory a test created, removed again when the last reference to it goes away. Every path it answers
/// is inside it; every raw helper acts inside it.
public final class TemporaryDirectory {
    /// The directory's absolute path, with no symlink in it (macOS reaches its temporary directories through
    /// one), so the paths a test builds from it compare equal to what the system reports.
    public let path: String

    private let files = FileManager()  // reach-ok: the tests' own file system, inside this directory only

    private init(path: String) {
        self.path = path
    }

    deinit {
        // Access is given back first, so a directory a test locked (mode 0o000) can be emptied and removed.
        restoreAccess(at: path)
        try? files.removeItem(atPath: path)
    }

    /// Creates a fresh, uniquely named directory under the system's temporary directory.
    public static func create() throws(FixtureFailure) -> TemporaryDirectory {
        let systemDirectory = NSTemporaryDirectory()  // reach-ok: where a test's own directory is created
        let separator = systemDirectory.hasSuffix("/") ? "" : "/"
        var template = Array((systemDirectory + separator + "bc-file-system-case-XXXXXX").utf8CString)
        guard mkdtemp(&template) != nil else {
            throw FixtureFailure(description: "mkdtemp in \(systemDirectory): errno \(errno)")
        }
        // mkdtemp wrote the name it chose into the template; the last element is the terminating NUL.
        let created = String(decoding: template.dropLast().map { UInt8(bitPattern: $0) }, as: UTF8.self)
        let resolved = URL(fileURLWithPath: created).resolvingSymlinksInPath().path
        return TemporaryDirectory(path: resolved)
    }

    /// The absolute path of an entry inside the directory.
    public func absolutePath(of relative: String) -> String {
        path + "/" + relative
    }

    // MARK: - Raw setup and raw checks

    /// Creates or replaces the file at `relative` with `text`, with exactly `mode`.
    public func rawWrite(_ text: String, to relative: String, mode: UInt32) throws(FixtureFailure) {
        let target = absolutePath(of: relative)
        guard files.createFile(atPath: target, contents: Data(text.utf8), attributes: nil) else {
            throw FixtureFailure(description: "could not write \(target)")
        }
        try rawSetMode(mode, of: relative)
    }

    /// The content of the file at `relative`, as text.
    public func rawRead(_ relative: String) throws(FixtureFailure) -> String {
        let target = absolutePath(of: relative)
        guard let data = files.contents(atPath: target) else {
            throw FixtureFailure(description: "could not read \(target)")
        }
        return String(decoding: data, as: UTF8.self)
    }

    /// Creates one directory with exactly `mode`.
    public func rawMakeDirectory(_ relative: String, mode: UInt32) throws(FixtureFailure) {
        let target = absolutePath(of: relative)
        do {
            try files.createDirectory(atPath: target, withIntermediateDirectories: false, attributes: nil)
        } catch {
            throw FixtureFailure(description: "mkdir \(target): \(error)")
        }
        try rawSetMode(mode, of: relative)
    }

    /// Creates the symlink at `relative` pointing at `target`, which is passed to the system as spelled.
    public func rawMakeSymlink(at relative: String, pointingAt target: String) throws(FixtureFailure) {
        let link = absolutePath(of: relative)
        do {
            try files.createSymbolicLink(atPath: link, withDestinationPath: target)
        } catch {
            throw FixtureFailure(description: "symlink \(link) -> \(target): \(error)")
        }
    }

    public func rawSetMode(_ mode: UInt32, of relative: String) throws(FixtureFailure) {
        let target = absolutePath(of: relative)
        do {
            try files.setAttributes([.posixPermissions: Int(mode)], ofItemAtPath: target)
        } catch {
            throw FixtureFailure(description: "chmod \(target): \(error)")
        }
    }

    /// The permission bits of the entry at `relative` itself (a symlink is not followed).
    public func rawMode(of relative: String) throws(FixtureFailure) -> UInt32 {
        let target = absolutePath(of: relative)
        guard let permissions = try rawAttributes(of: target)[.posixPermissions] as? Int else {
            throw FixtureFailure(description: "no permission bits reported for \(target)")
        }
        return UInt32(permissions) & 0o7777
    }

    /// Whether anything is at `relative` (a symlink counts, whatever it points at).
    public func rawExists(_ relative: String) -> Bool {
        (try? rawAttributes(of: absolutePath(of: relative))) != nil
    }

    /// Every name directly inside the directory, hidden ones included, sorted.
    public func rawNames() throws(FixtureFailure) -> [String] {
        do {
            return try files.contentsOfDirectory(atPath: path).sorted()
        } catch {
            throw FixtureFailure(description: "could not list \(path): \(error)")
        }
    }

    /// False when the process may read what its permission bits forbid, as the superuser may. A test of a
    /// permission failure returns early there, because no such failure can happen.
    public func permissionsAreEnforced() throws(FixtureFailure) -> Bool {
        let probe = ".permission-probe"
        try rawWrite("probe", to: probe, mode: 0o000)
        defer { try? files.removeItem(atPath: absolutePath(of: probe)) }
        return files.contents(atPath: absolutePath(of: probe)) == nil
    }

    /// Every file under `directory` whose name ends in `suffix`, as (path, text), for the source scan, which
    /// reads the package's own sources through this one legal site.
    public static func textFiles(under directory: String, suffix: String) throws(FixtureFailure) -> [(
        path: String, text: String
    )] {
        let files = FileManager()  // reach-ok: the source scan reads the package's own files through this site
        guard let walk = files.enumerator(atPath: directory) else {
            throw FixtureFailure(description: "could not walk \(directory)")
        }
        var found: [(path: String, text: String)] = []
        for case let relative as String in walk where relative.hasSuffix(suffix) {
            let path = directory + "/" + relative
            guard let data = files.contents(atPath: path) else {
                throw FixtureFailure(description: "could not read \(path)")
            }
            found.append((path, String(decoding: data, as: UTF8.self)))
        }
        return found.sorted { $0.path < $1.path }
    }

    // The attributes of the entry itself: FileManager reads them with lstat, so a symlink is reported as one.
    private func rawAttributes(of target: String) throws(FixtureFailure) -> [FileAttributeKey: Any] {
        do {
            return try files.attributesOfItem(atPath: target)
        } catch {
            throw FixtureFailure(description: "lstat \(target): \(error)")
        }
    }

    // Gives the owner back full access to `target` and everything below it.
    private func restoreAccess(at target: String) {
        guard let attributes = try? files.attributesOfItem(atPath: target),
            attributes[.type] as? FileAttributeType == .typeDirectory
        else {
            return
        }
        try? files.setAttributes([.posixPermissions: 0o700], ofItemAtPath: target)
        for name in (try? files.contentsOfDirectory(atPath: target)) ?? [] {
            restoreAccess(at: target + "/" + name)
        }
    }
}
