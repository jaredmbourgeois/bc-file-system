import CBCFileSystem

// The face's own compositions: the buffer protocol's grow loop, `readFileIfPresent` and line iteration. They
// are the only logic the face holds; every operation is otherwise one call into the core.

/// The first capacity offered to the core for an answer of variable length. A larger answer costs one more call,
/// since the core names the capacity it needs, and that call resolves, opens and measures again.
///
/// A file's bytes: 1 MiB, so one call serves every file up to 1 MiB, read into the answer itself and never
/// zero-filled first (the core writes what it answers). Measured at stage 28 through the core (macOS, APFS, median
/// µs, one-minute load 57 to 95), against a 64 KiB first offer: a 4 KiB file 12.96 against 12.92, a 64 KiB file
/// 17.12 against 15.88 (the copy to its size), a 256 KiB file 28.08 against 38.50, a 1 MiB file 43.04 against
/// 59.83; the 64 KiB offer's zero fill had cost 0.71 µs more at 4 KiB and 7.88 µs more at 1 MiB. A larger first
/// offer is a fresh mapping on every call on macOS: 16 MiB made a 1 MiB read 68.46 against 43.04.
let firstReadCapacity = 1024 * 1024
let firstTextCapacity = 1024
/// A listing: room for 1,024 entries (264 KiB), never zero-filled. Measured at stage 28 through the core, with
/// the core reading the directory once (same probe, median µs): an empty directory 18.12 against 18.21 for 64
/// entries, 10 entries 25.88 against 25.92, 1,000 entries 907.67 against 1,703.62 (64 and 256 entries both
/// overflow there and pay a second call). Zero-filled, the 1,024-entry offer cost 2.3 µs more on every listing.
let firstEntryCapacity = 1024
/// The grow loop's bound, so a file that grows between two calls cannot livelock the face: after this many
/// attempts the `bufferTooSmall` is thrown as received.
let mostAttempts = 3

/// The buffer protocol as one loop. `call` is the core's function with everything but the buffer already bound:
/// it receives the buffer, its capacity, the length to fill and the error to fill, and answers the core's
/// status. The buffer is never zero-filled: the core writes what it answers, and only what it stored is ever
/// counted, so nothing of the buffer past it can reach the answer. An answer filling less than half its buffer
/// comes back copied into an array of its own size, so a small file read through the 1 MiB first offer holds no
/// more than it needs.
func grownAnswer(
    firstCapacity: Int,
    operation: Operation,
    locationText: [UInt8],
    call: (UnsafeMutablePointer<UInt8>?, Int, inout Int, inout bc_file_system_error) -> Int32
) throws(FileSystemError) -> [UInt8] {
    var capacity = firstCapacity
    var coreError = bc_file_system_error()
    let bufferTooSmall = Int32(bc_file_system_error_kind_buffer_too_small.rawValue)
    for attempt in 1...mostAttempts {
        var outcome: Int32 = 0
        var length = 0
        let buffer = [UInt8](unsafeUninitializedCapacity: capacity) { uninitialized, initializedCount in
            outcome = call(uninitialized.baseAddress, capacity, &length, &coreError)
            // Only what the core stored is ever counted as initialised: on success the `length` bytes at the
            // start of the buffer, which the header holds within the capacity offered; on any failure nothing.
            initializedCount = outcome == 0 ? min(length, capacity) : 0
        }
        if outcome == 0 {
            if buffer.capacity / 2 > buffer.count {
                return buffer.withUnsafeBufferPointer { stored in Array(stored) }
            }
            return buffer
        }
        if outcome == bufferTooSmall && attempt < mostAttempts {
            capacity = length
            continue
        }
        throw FileSystemError(core: coreError, called: operation, locationText: locationText)
    }
    // The loop returns or throws on its last attempt; this line is never reached.
    throw FileSystemError(core: coreError, called: operation, locationText: locationText)
}

extension FileSystem {
    /// `readFile`, with absence as nil instead of `notFound`.
    public func readFileIfPresent(_ location: Location, options: ReadOptions) throws(FileSystemError) -> [UInt8]? {
        do {
            return try readFile(location, options: options)
        } catch {
            if error.kind == .notFound {
                return nil
            }
            throw error
        }
    }
}

/// The lines of a read buffer, each a slice of it without its newline: split on `\n` only, the bytes untouched
/// (a `\r` stays where it was; the library never decides what text the bytes are). A final line without a
/// newline is a line; a trailing newline does not add an empty one; an empty buffer has no lines.
public func lines(of buffer: [UInt8]) -> [ArraySlice<UInt8>] {
    let newline = UInt8(ascii: "\n")
    var lines: [ArraySlice<UInt8>] = []
    var start = buffer.startIndex
    while start < buffer.endIndex {
        let end: Int
        if let found = buffer[start...].firstIndex(of: newline) {
            end = found
        } else {
            end = buffer.endIndex
        }
        lines.append(buffer[start..<end])
        start = end + 1
    }
    return lines
}
