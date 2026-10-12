import CBCFileSystem

/// The context every operation runs in: the platform's probed fast paths and, for tests, the observer. Created
/// by `live()` at a composition root, or by the fixtures' observed factory in a test, and handed down inside a
/// `FileSystem`.
///
/// Naming `live()` is itself a reach: it is legal only at a composition root.
///
/// Why `@unchecked Sendable` is sound: the one stored property is the C handle, a `let` set once in `init` and
/// never reassigned; the core promises (the header's THREADS paragraph) that a live engine is immutable after
/// creation and may be used from any number of threads at once; every call into the core goes through
/// `withHandle`, which lends the handle for the duration of one call and never stores it elsewhere; and the
/// handle is released exactly once, in `deinit`, when the last reference goes away, so no call can outlive it.
/// An engine created for tests records every step and is used from one thread at a time, which the test that
/// created it owns.
public final class Engine: @unchecked Sendable {
    private let handle: OpaquePointer

    /// Adopts a handle one of the two C factories created. `package`, so the fixtures' observed factory can
    /// build an engine without this module naming the test-only factory.
    package init(adopting handle: OpaquePointer) {
        self.handle = handle
    }

    deinit {
        bc_file_system_engine_release(handle)
    }

    /// The live engine of a composition root: probes the platform's fast paths once and carries no observer.
    public static func live() throws(FileSystemError) -> Engine {
        var created: OpaquePointer?
        var coreError = bc_file_system_error()
        if bc_file_system_engine_create_live(&created, &coreError) != 0 {
            throw FileSystemError(core: coreError, called: .engineCreateLive, locationText: [])
        }
        guard let handle = created else {
            // The core answered success and stored nothing, which its contract rules out.
            throw FileSystemError.answerOutsideEnum(called: .engineCreateLive, locationText: [])
        }
        return Engine(adopting: handle)
    }

    /// Lends the handle to one call into the core. The one way any code reaches the handle.
    package func withHandle<T>(_ body: (OpaquePointer) -> T) -> T {
        body(handle)
    }
}
