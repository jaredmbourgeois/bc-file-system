//! The observed engine and the faults it takes.

use bc_file_system::{Engine, Error, Operation, Step, UnknownValue, ffi, testing_support};

/// The engine with the observer installed: every step is counted, and the faults scripted with
/// [`ObservedEngine::add_fault`] take effect at the step they name. Test code only, which the house-rules guard
/// enforces in this repository.
///
/// The header says an engine created for tests is used from one thread at a time; the engine itself keeps that
/// promise (every call into it takes its turn), so this value may be shared between a test's threads and every
/// method takes `&self`.
#[derive(Debug)]
pub struct ObservedEngine {
    engine: Engine,
}

impl ObservedEngine {
    /// The observed engine over the fast paths the platform's probe found: one of the two ways to install the
    /// observer.
    ///
    /// # Errors
    ///
    /// `ResourceExhausted` when the engine cannot be allocated.
    pub fn observed_for_tests() -> Result<Self, Error> {
        Ok(Self {
            engine: testing_support::observed_for_tests()?,
        })
    }

    /// The observed engine with every fast path refused, the other way to install the observer: every operation
    /// takes the path every platform has (the walk, the read-and-write loop, fsync), and one whose promise a fast
    /// path alone keeps (the no-replace rename of a `Refuse` and of a condemn) answers `Unsupported`, as a
    /// platform without that path does. For running a test a second time, to prove the fallbacks on a platform
    /// that has every fast path.
    ///
    /// # Errors
    ///
    /// `ResourceExhausted` when the engine cannot be allocated.
    pub fn portable_only_for_tests() -> Result<Self, Error> {
        Ok(Self {
            engine: testing_support::portable_only_for_tests()?,
        })
    }

    /// The engine to run operations on, handed to the code under test as a live engine would be.
    #[must_use]
    pub fn engine(&self) -> &Engine {
        &self.engine
    }

    /// Scripts one fault. The slot holds sixty-four.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for an occurrence of 0; `ResourceExhausted` when the slot is full.
    pub fn add_fault(&self, fault: Fault) -> Result<(), Error> {
        testing_support::add_fault(&self.engine, &fault.to_c())
    }

    /// Forgets every scripted fault; the step counts are kept.
    ///
    /// # Errors
    ///
    /// None for an observed engine; the signature is the core's one return convention.
    pub fn clear_faults(&self) -> Result<(), Error> {
        testing_support::clear_faults(&self.engine)
    }

    /// How many times `operation` has reached `step` in this engine's lifetime.
    ///
    /// # Errors
    ///
    /// None for an observed engine; the signature is the core's one return convention.
    pub fn step_count(&self, operation: Operation, step: Step) -> Result<u64, Error> {
        testing_support::step_count(&self.engine, operation, step)
    }
}

/// What a scripted fault does when its step is reached.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FaultAction {
    /// The step fails with `system_code` (an errno, at least 1), exactly as if the system had failed the call.
    Fail {
        /// The errno the step answers.
        system_code: i32,
    },
    /// The operation returns from the step without any cleanup, as if the process had been killed there: how
    /// every multi-step operation's kill points are tested.
    Stop,
}

/// One scripted fault: the `occurrence`-th time `operation` reaches `step`, `action` happens. The action carries
/// its errno, so a `fail` without a code or a `stop` with one cannot be spelled.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Fault {
    /// The operation the fault waits for.
    pub operation: Operation,
    /// The system call the fault waits for.
    pub step: Step,
    /// Counted from 1 across the engine's lifetime.
    pub occurrence: u32,
    /// What happens when the step is reached.
    pub action: FaultAction,
}

impl Fault {
    /// The fault as the core takes it.
    #[must_use]
    pub const fn to_c(self) -> ffi::bc_file_system_fault {
        let (action, system_code) = match self.action {
            FaultAction::Fail { system_code } => (ffi::FAULT_ACTION_FAIL, system_code),
            FaultAction::Stop => (ffi::FAULT_ACTION_STOP, 0),
        };
        ffi::bc_file_system_fault {
            operation: self.operation as i32,
            step: self.step as i32,
            occurrence: self.occurrence,
            action,
            system_code,
        }
    }

    /// The core's fault struct as the typed value: the one reading of its integers, refusing a value outside
    /// its enum.
    ///
    /// # Errors
    ///
    /// [`UnknownValue`] for an operation, a step or an action outside its enum.
    pub fn from_c(c_fault: &ffi::bc_file_system_fault) -> Result<Self, UnknownValue> {
        let action = match c_fault.action {
            ffi::FAULT_ACTION_FAIL => FaultAction::Fail {
                system_code: c_fault.system_code,
            },
            ffi::FAULT_ACTION_STOP => FaultAction::Stop,
            other => {
                return Err(UnknownValue {
                    enumeration: "bc_file_system_fault_action",
                    value: other,
                });
            }
        };
        Ok(Self {
            operation: Operation::try_from(c_fault.operation)?,
            step: Step::try_from(c_fault.step)?,
            occurrence: c_fault.occurrence,
            action,
        })
    }
}
