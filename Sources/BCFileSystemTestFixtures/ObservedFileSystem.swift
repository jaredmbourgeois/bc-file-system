import BCFileSystem
import CBCFileSystem

// The observed engine: the same engine as the live one with the observer installed, so every step is counted
// and scripted faults take effect. Created by one factory, the only way to install an observer, which the
// house-rules guard confines to test code.

/// What a scripted fault does when its step is reached.
public enum FaultAction: Int32, CaseIterable, Sendable {
    /// The step fails with the fault's `systemCode`, exactly as if the system had failed the call.
    case fail = 1
    /// The operation returns from the step without any cleanup, as if the process had been killed there: how
    /// every multi-step operation's kill points are tested.
    case stop = 2
}

/// One scripted fault: the `occurrence`-th time `operation` reaches `step`, `action` happens.
public struct Fault: Equatable, Sendable {
    public let operation: Operation
    public let step: Step
    /// Counted from 1 across the engine's lifetime.
    public let occurrence: UInt32
    public let action: FaultAction
    /// The errno to answer under `fail` (at least 1); 0 under `stop`.
    public let systemCode: Int32

    public init(operation: Operation, step: Step, occurrence: UInt32, action: FaultAction, systemCode: Int32) {
        self.operation = operation
        self.step = step
        self.occurrence = occurrence
        self.action = action
        self.systemCode = systemCode
    }

    public var core: bc_file_system_fault {
        bc_file_system_fault(
            operation: operation.rawValue,
            step: step.rawValue,
            occurrence: occurrence,
            action: action.rawValue,
            system_code: systemCode
        )
    }

    /// The fault the core holds, or nil when one of its fields is outside its enum.
    public init?(core: bc_file_system_fault) {
        guard let operation = Operation(rawValue: core.operation),
            let step = Step(rawValue: core.step),
            let action = FaultAction(rawValue: core.action)
        else {
            return nil
        }
        self.init(
            operation: operation, step: step, occurrence: core.occurrence, action: action, systemCode: core.system_code)
    }
}

/// A file system over an observed engine, in a directory the test created: the file system to act through,
/// the directory to act in, the faults to script and the step counts to read.
public struct ObservedFileSystem {
    public let fileSystem: FileSystem
    public let root: TemporaryDirectory

    init(fileSystem: FileSystem, root: TemporaryDirectory) {
        self.fileSystem = fileSystem
        self.root = root
    }

    /// An `at` location inside the test's directory.
    public func location(_ relative: String) throws(FileSystemError) -> Location {
        .at(try AbsolutePath(validating: root.absolutePath(of: relative)))
    }

    /// Scripts one fault into the engine; a full slot is `resourceExhausted`.
    public func addFault(_ fault: Fault) throws(FileSystemError) {
        let coreFault = fault.core
        var coreError = bc_file_system_error()
        let outcome = fileSystem.engine.withHandle { handle in
            withUnsafePointer(to: coreFault) { faultPointer in
                bc_file_system_observed_fault_add(handle, faultPointer, &coreError)
            }
        }
        if outcome != 0 {
            throw FileSystemError(core: coreError, called: .observedFaultAdd, locationText: [])
        }
    }

    /// Forgets every scripted fault; the step counts are kept.
    public func clearFaults() throws(FileSystemError) {
        var coreError = bc_file_system_error()
        let outcome = fileSystem.engine.withHandle { handle in
            bc_file_system_observed_faults_clear(handle, &coreError)
        }
        if outcome != 0 {
            throw FileSystemError(core: coreError, called: .observedFaultsClear, locationText: [])
        }
    }

    /// How many times `operation` has reached `step` in the engine's lifetime.
    public func stepCount(operation: Operation, step: Step) throws(FileSystemError) -> UInt64 {
        var count: UInt64 = 0
        var coreError = bc_file_system_error()
        let outcome = fileSystem.engine.withHandle { handle in
            bc_file_system_observed_step_count(handle, operation.rawValue, step.rawValue, &count, &coreError)
        }
        if outcome != 0 {
            throw FileSystemError(core: coreError, called: .observedStepCount, locationText: [])
        }
        return count
    }
}

extension FileSystem {
    /// The file system of a test: an engine with the observer installed, over the directory the test created.
    public static func observed(root: TemporaryDirectory) throws(FileSystemError) -> ObservedFileSystem {
        var created: OpaquePointer?
        var coreError = bc_file_system_error()
        if bc_file_system_engine_create_observed_for_tests(&created, &coreError) != 0 {
            throw FileSystemError(core: coreError, called: .engineCreateObservedForTests, locationText: [])
        }
        guard let handle = created else {
            throw FileSystemError.answerOutsideEnum(called: .engineCreateObservedForTests, locationText: [])
        }
        return ObservedFileSystem(fileSystem: FileSystem(engine: Engine(adopting: handle)), root: root)
    }
}
