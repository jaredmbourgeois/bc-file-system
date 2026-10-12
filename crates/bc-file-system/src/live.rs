//! The engine handles and the calls into the core: the one module beside `ffi` that holds `unsafe`.
//!
//! [`Engine::live`] is the Rust face's live seam: naming it is itself a reach, legal only at a composition root
//! (AGENTS.md, "Inject, never reach"), which is why this file is the Rust entry of `REACH_IS_LEGAL_IN` in
//! `scripts/check_house_rules.py`. [`ObservedEngine::observed_for_tests`] is the only way to install the
//! observer, confined by the guard's `test-only-factory` leg to test code until `bc-file-system-testing` takes
//! it at stage `rust-crates`. Every `unsafe` block restates the header's pointer contract it relies on. The
//! value types that one operation needs this stage (`FinalSymlink`, `EntryType`, `Status`) live here until the
//! typed face grows its own modules at that stage.
#![expect(
    unsafe_code,
    reason = "the calls into the C core and the two thread-safety proofs; nowhere else"
)]

use core::ptr::NonNull;

use crate::error::{Error, Operation, Step, UnknownValue};
use crate::ffi;

/// The context every operation runs in: the platform's fast paths, probed once at creation, and no observer.
/// Created at a composition root, handed down by reference, released when dropped.
///
/// `Send` and `Sync`: the header's THREADS paragraph promises that a live engine is immutable after creation and
/// may be used from any number of threads at once, so a shared reference may cross threads and the handle may
/// move; the proofs are the two `unsafe impl`s below, which restate exactly that.
#[derive(Debug)]
pub struct Engine {
    handle: NonNull<ffi::bc_file_system_engine>,
}

// SAFETY: a live engine is immutable after creation (the header, THREADS); the core writes nothing through
// the handle on any live call, so moving it to another thread changes nothing.
unsafe impl Send for Engine {}
// SAFETY: as above; concurrent live calls read the same immutable probes and share nothing mutable.
unsafe impl Sync for Engine {}

/// One of the two factories, called with the operation it reports under.
fn create(
    factory: unsafe extern "C" fn(
        *mut *mut ffi::bc_file_system_engine,
        *mut ffi::bc_file_system_error,
    ) -> i32,
    called: Operation,
) -> Result<NonNull<ffi::bc_file_system_engine>, Error> {
    let mut handle: *mut ffi::bc_file_system_engine = core::ptr::null_mut();
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `handle` and `c_error` are live locals for the call, which is the header's whole contract for
    // the two pointers; the core writes the handle only on success and the error only on failure.
    let status = unsafe { factory(&raw mut handle, &raw mut c_error) };
    if status != 0 {
        return Err(Error::from_c(&c_error, called));
    }
    NonNull::new(handle).ok_or_else(|| Error::from_c(&c_error, called))
}

impl Engine {
    /// The live engine of a composition root, with the platform's fast paths probed once.
    ///
    /// # Errors
    ///
    /// `ResourceExhausted` when the engine cannot be allocated; the probes themselves cannot fail.
    pub fn live() -> Result<Self, Error> {
        create(
            ffi::bc_file_system_engine_create_live,
            Operation::EngineCreateLive,
        )
        .map(|handle| Self { handle })
    }

    /// What is at `absolute_path`, a valid absolute path as the header defines one, judged with or without
    /// following a final symlink. Absence is `None`, not an error.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for text that is no absolute path; `Unsupported` until the operation lands in the core
    /// (stage `status-and-reads`); afterwards the kinds the header lists for `bc_file_system_status_of`.
    pub fn status_of(
        &self,
        absolute_path: &[u8],
        final_symlink: FinalSymlink,
    ) -> Result<Option<Status>, Error> {
        let location = ffi::bc_file_system_location_at(absolute_path.as_ptr(), absolute_path.len());
        let mut present: i32 = 0;
        let mut c_status = ffi::bc_file_system_status::zeroed();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        // SAFETY: the engine handle is live while `self` is; `location` borrows `absolute_path` for the call;
        // `present`, `c_status` and `c_error` are live locals, written only on success or, for the error, on
        // failure, as the header states.
        let status = unsafe {
            ffi::bc_file_system_status_of(
                self.handle.as_ptr(),
                &raw const location,
                final_symlink as i32,
                &raw mut present,
                &raw mut c_status,
                &raw mut c_error,
            )
        };
        if status != 0 {
            return Err(Error::from_c(&c_error, Operation::Status));
        }
        if present == 0 {
            return Ok(None);
        }
        Status::from_c(&c_status).map(Some)
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        // SAFETY: the handle came from a factory and is released exactly once, here; nothing uses it afterwards.
        unsafe { ffi::bc_file_system_engine_release(self.handle.as_ptr()) }
    }
}

/// The same engine with the observer installed: every step is counted, and the faults scripted with
/// [`ObservedEngine::add_fault`] take effect at the step they name. Test code only, which the house-rules guard
/// enforces. The header says an observed engine is used from one thread at a time; the receivers say it here:
/// everything that writes takes `&mut self`, and the operations run through [`ObservedEngine::engine`], which
/// lends the engine exclusively.
#[derive(Debug)]
pub struct ObservedEngine {
    engine: Engine,
}

impl ObservedEngine {
    /// The only way to install the observer.
    ///
    /// # Errors
    ///
    /// `ResourceExhausted` when the engine cannot be allocated.
    pub fn observed_for_tests() -> Result<Self, Error> {
        create(
            ffi::bc_file_system_engine_create_observed_for_tests,
            Operation::EngineCreateObservedForTests,
        )
        .map(|handle| Self {
            engine: Engine { handle },
        })
    }

    /// The engine to run operations on, lent exclusively so no step is counted from two threads at once.
    pub fn engine(&mut self) -> &mut Engine {
        &mut self.engine
    }

    /// Scripts one fault. The slot holds sixty-four.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for an occurrence of 0; `ResourceExhausted` when the slot is full.
    pub fn add_fault(&mut self, fault: Fault) -> Result<(), Error> {
        let c_fault = fault.to_c();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        // SAFETY: the handle is live while `self` is; `c_fault` is a live local the core copies; `c_error` is a
        // live local written on failure.
        let status = unsafe {
            ffi::bc_file_system_observed_fault_add(
                self.engine.handle.as_ptr(),
                &raw const c_fault,
                &raw mut c_error,
            )
        };
        if status != 0 {
            return Err(Error::from_c(&c_error, Operation::ObservedFaultAdd));
        }
        Ok(())
    }

    /// Forgets every scripted fault; the step counts are kept.
    ///
    /// # Errors
    ///
    /// None for an observed engine; the signature is the core's one return convention.
    pub fn clear_faults(&mut self) -> Result<(), Error> {
        let mut c_error = ffi::bc_file_system_error::zeroed();
        // SAFETY: the handle is live while `self` is; `c_error` is a live local written on failure.
        let status = unsafe {
            ffi::bc_file_system_observed_faults_clear(self.engine.handle.as_ptr(), &raw mut c_error)
        };
        if status != 0 {
            return Err(Error::from_c(&c_error, Operation::ObservedFaultsClear));
        }
        Ok(())
    }

    /// How many times `operation` has reached `step` in this engine's lifetime.
    ///
    /// # Errors
    ///
    /// None for an observed engine; the signature is the core's one return convention.
    pub fn step_count(&self, operation: Operation, step: Step) -> Result<u64, Error> {
        let mut count: u64 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        // SAFETY: the handle is live while `self` is; `count` and `c_error` are live locals, the first written on
        // success and the second on failure.
        let status = unsafe {
            ffi::bc_file_system_observed_step_count(
                self.engine.handle.as_ptr(),
                operation as i32,
                step as i32,
                &raw mut count,
                &raw mut c_error,
            )
        };
        if status != 0 {
            return Err(Error::from_c(&c_error, Operation::ObservedStepCount));
        }
        Ok(count)
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

impl FaultAction {
    /// The header's `bc_file_system_fault_action` value.
    const fn to_c(self) -> i32 {
        match self {
            Self::Fail { .. } => 1,
            Self::Stop => 2,
        }
    }
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
        let system_code = match self.action {
            FaultAction::Fail { system_code } => system_code,
            FaultAction::Stop => 0,
        };
        ffi::bc_file_system_fault {
            operation: self.operation as i32,
            step: self.step as i32,
            occurrence: self.occurrence,
            action: self.action.to_c(),
            system_code,
        }
    }
}

/// What to do when the last component of a path is a symlink.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FinalSymlink {
    /// The symlink itself is the entry: it is reported, removed or refused, never followed.
    Refuse = 1,
    /// The operation acts on what the symlink points at.
    Follow = 2,
}

/// What an entry is, judged without following a symlink.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EntryType {
    /// A regular file.
    RegularFile = 1,
    /// A directory.
    Directory = 2,
    /// A symlink, itself.
    Symlink = 3,
    /// A socket, a FIFO, a device.
    Other = 4,
}

impl EntryType {
    /// Every entry type, in the header's order.
    pub const ALL: [Self; 4] = [
        Self::RegularFile,
        Self::Directory,
        Self::Symlink,
        Self::Other,
    ];
}

impl TryFrom<i32> for EntryType {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        Self::ALL
            .iter()
            .copied()
            .find(|variant| *variant as i32 == value)
            .ok_or(UnknownValue {
                enumeration: "bc_file_system_entry_type",
                value,
            })
    }
}

/// What the file system records about one entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Status {
    /// What the entry is.
    pub entry_type: EntryType,
    /// A regular file's size in bytes; 0 for every other type.
    pub size: i64,
    /// The permission bits (0644), without the file-type bits.
    pub mode: u32,
    /// `st_dev`: with `inode`, the entry's identity.
    pub device: u64,
    /// `st_ino`.
    pub inode: u64,
    /// `st_nlink`.
    pub link_count: u64,
}

impl Status {
    /// The core's status as the typed value. An entry type outside the enum is a core/face mismatch, reported
    /// as `InvalidArgument` under `Status` with the raw value kept, like every unknown value.
    fn from_c(c_status: &ffi::bc_file_system_status) -> Result<Self, Error> {
        let entry_type = EntryType::try_from(c_status.entry_type).map_err(|unknown| Error {
            kind: crate::ErrorKind::InvalidArgument,
            operation: Operation::Status,
            step: None,
            system_code: 0,
            name: Vec::new(),
            name_truncated: false,
            unknown_value: Some(unknown),
        })?;
        Ok(Self {
            entry_type,
            size: c_status.size,
            mode: c_status.mode,
            device: c_status.device,
            inode: c_status.inode,
            link_count: c_status.link_count,
        })
    }
}
