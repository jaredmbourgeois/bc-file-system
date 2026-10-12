import CBCFileSystem

// The face's own compositions: the buffer protocol's grow loop, `readFileIfPresent` and line iteration. They
// are the only logic the face holds; every operation is otherwise one call into the core.

/// The first capacity offered to the core for an answer of variable length. One read serves most files and
/// names outright; a larger answer costs one more call, since the core names the capacity it needs.
let firstReadCapacity = 64 * 1024
let firstTextCapacity = 1024
let firstEntryCapacity = 64
/// The grow loop's bound, so a file that grows between two calls cannot livelock the face: after this many
/// attempts the `bufferTooSmall` is thrown as received.
let mostAttempts = 3

/// The buffer protocol as one loop. `call` is the core's function with everything but the buffer already bound:
/// it receives the buffer, its capacity, the length to fill and the error to fill, and answers the core's
/// status. The answer comes back sized to what the core stored.
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
        var answer = [UInt8](repeating: 0, count: capacity)
        var length = 0
        let outcome = answer.withUnsafeMutableBufferPointer { buffer in
            call(buffer.baseAddress, buffer.count, &length, &coreError)
        }
        if outcome == 0 {
            return Array(answer.prefix(length))
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
