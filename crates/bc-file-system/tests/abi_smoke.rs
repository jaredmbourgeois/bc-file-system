//! The ABI smoke: the round trip through the C core that proves the Rust declarations against the real thing.
//! Every Rust-side test can pass while the scaffolding is wrong, until a value crosses; here one does, in each
//! direction: an engine is created and released, faults and counts cross into an observed engine, every enum
//! value round-trips through its integer, the layout pins hold at run time as well as at compile time, and an
//! error travels whole from the core's stub to the typed value.

use bc_file_system::ffi;
use bc_file_system::{
    Engine, EntryType, Error, ErrorKind, Fault, FaultAction, FinalSymlink, ObservedEngine,
    Operation, Step, UnknownValue,
};

#[test]
fn a_live_engine_is_created_and_released_by_drop() {
    let engine = Engine::live().expect("a live engine");
    drop(engine);
}

#[test]
fn a_live_engine_may_be_shared_and_moved_between_threads() {
    fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<Engine>();
    assert_send_and_sync::<ObservedEngine>();
}

#[test]
fn an_observed_engine_takes_a_fault_counts_nothing_yet_and_clears() {
    let mut observed = ObservedEngine::observed_for_tests().expect("an observed engine");
    let fault = Fault {
        operation: Operation::WriteFile,
        step: Step::Write,
        occurrence: 1,
        action: FaultAction::Fail { system_code: 28 },
    };

    observed.add_fault(fault).expect("the fault is scripted");
    let count = observed
        .step_count(Operation::WriteFile, Step::Write)
        .expect("the count is read");
    observed.clear_faults().expect("the faults are cleared");
    let after_clearing = observed
        .step_count(Operation::WriteFile, Step::Write)
        .expect("the count is read");

    assert_eq!(count, 0, "no operation has made a step yet");
    assert_eq!(after_clearing, 0, "clearing keeps the counts");
}

#[test]
fn a_fault_with_occurrence_zero_is_refused_with_the_operation_named() {
    let mut observed = ObservedEngine::observed_for_tests().expect("an observed engine");
    let fault = Fault {
        operation: Operation::WriteFile,
        step: Step::Write,
        occurrence: 0,
        action: FaultAction::Stop,
    };

    let refused = observed
        .add_fault(fault)
        .expect_err("occurrence 0 is a contract violation");

    assert_eq!(refused.kind, ErrorKind::InvalidArgument);
    assert_eq!(refused.operation, Operation::ObservedFaultAdd);
    assert_eq!(refused.step, None);
    assert_eq!(refused.system_code, 0);
    assert!(refused.name.is_empty());
    assert_eq!(refused.unknown_value, None);
}

#[test]
fn a_fault_crosses_as_the_header_lays_it_out() {
    let fail = Fault {
        operation: Operation::Rename,
        step: Step::Renameat,
        occurrence: 3,
        action: FaultAction::Fail { system_code: 5 },
    };
    let stop = Fault {
        operation: Operation::WriteFile,
        step: Step::Fsync,
        occurrence: 2,
        action: FaultAction::Stop,
    };

    let c_fail = fail.to_c();
    let c_stop = stop.to_c();

    assert_eq!(
        (
            c_fail.operation,
            c_fail.step,
            c_fail.occurrence,
            c_fail.action,
            c_fail.system_code
        ),
        (20, 10, 3, 1, 5)
    );
    assert_eq!(
        (
            c_stop.operation,
            c_stop.step,
            c_stop.occurrence,
            c_stop.action,
            c_stop.system_code
        ),
        (10, 8, 2, 2, 0)
    );
}

#[test]
fn the_layout_pins_hold_at_run_time() {
    // The same numbers the core asserts and ffi.rs pins at compile time; a run-time witness beside them.
    assert_eq!(size_of::<ffi::bc_file_system_error>(), 280);
    assert_eq!(core::mem::offset_of!(ffi::bc_file_system_error, name), 21);
    assert_eq!(
        size_of::<ffi::bc_file_system_location>(),
        if size_of::<usize>() == 8 { 32 } else { 16 }
    );
    assert_eq!(size_of::<ffi::bc_file_system_read_limit>(), 16);
    assert_eq!(size_of::<ffi::bc_file_system_permissions>(), 8);
    assert_eq!(size_of::<ffi::bc_file_system_write_options>(), 20);
    assert_eq!(size_of::<ffi::bc_file_system_read_options>(), 24);
    assert_eq!(size_of::<ffi::bc_file_system_status>(), 48);
    assert_eq!(size_of::<ffi::bc_file_system_entry>(), 264);
    assert_eq!(size_of::<ffi::bc_file_system_fault>(), 20);
}

#[test]
fn every_error_kind_round_trips_through_its_integer() {
    for kind in ErrorKind::ALL {
        assert_eq!(
            ErrorKind::try_from(kind as i32),
            Ok(kind),
            "{}",
            kind.name()
        );
    }
    assert_eq!(ErrorKind::ALL.len(), 15);
    assert_eq!(ErrorKind::BufferTooSmall as i32, 15);
    assert_eq!(
        ErrorKind::try_from(0),
        Err(UnknownValue {
            enumeration: "bc_file_system_error_kind",
            value: 0
        })
    );
    assert!(ErrorKind::try_from(16).is_err());
}

#[test]
fn every_operation_round_trips_through_its_integer() {
    for operation in Operation::ALL {
        assert_eq!(
            Operation::try_from(operation as i32),
            Ok(operation),
            "{}",
            operation.name()
        );
    }
    assert_eq!(Operation::ALL.len(), 39);
    assert_eq!(Operation::PathLibraryName as i32, 39);
    assert!(Operation::try_from(0).is_err());
    assert!(Operation::try_from(40).is_err());
}

#[test]
fn every_step_round_trips_through_its_integer() {
    for step in Step::ALL {
        assert_eq!(Step::try_from(step as i32), Ok(step), "{}", step.name());
    }
    assert_eq!(Step::ALL.len(), 27);
    assert_eq!(Step::Faccessat as i32, 27);
    assert!(Step::try_from(0).is_err());
    assert!(Step::try_from(28).is_err());
}

#[test]
fn every_entry_type_round_trips_through_its_integer() {
    for entry_type in EntryType::ALL {
        assert_eq!(EntryType::try_from(entry_type as i32), Ok(entry_type));
    }
    assert!(EntryType::try_from(0).is_err());
    assert!(EntryType::try_from(5).is_err());
    assert_eq!(FinalSymlink::Refuse as i32, 1);
    assert_eq!(FinalSymlink::Follow as i32, 2);
}

// The one location every platform has and the smoke can name without a temporary directory of its own (the
// typed crate's fixtures arrive at stage `rust-crates`): the root, which is a directory wherever the core runs.
#[test]
fn a_status_call_answers_the_root_as_a_directory() {
    let engine = Engine::live().expect("a live engine");

    let status = engine
        .status_of(b"/", FinalSymlink::Refuse)
        .expect("the root is described")
        .expect("the root is present");

    assert_eq!(status.entry_type, EntryType::Directory);
    assert_eq!(status.size, 0, "a size is a regular file's only");
    assert!(status.link_count >= 1);
}

#[test]
fn a_status_call_through_an_observed_engine_travels_the_same_way_and_is_counted() {
    let mut observed = ObservedEngine::observed_for_tests().expect("an observed engine");

    let status = observed
        .engine()
        .status_of(b"/", FinalSymlink::Follow)
        .expect("the root is described")
        .expect("the root is present");
    let root_opens = observed
        .step_count(Operation::Status, Step::Open)
        .expect("the count is read");

    assert_eq!(status.entry_type, EntryType::Directory);
    assert_eq!(
        root_opens, 1,
        "resolving an absolute path opens the root once"
    );
}

#[test]
fn relative_text_under_at_is_refused_at_the_door() {
    let engine = Engine::live().expect("a live engine");

    let refused = engine
        .status_of(b"relative.txt", FinalSymlink::Refuse)
        .expect_err("relative text is refused");

    assert_eq!(refused.kind, ErrorKind::InvalidArgument);
    assert_eq!(refused.operation, Operation::Status);
    assert_eq!(refused.to_string(), "status: invalid_argument");
}

#[test]
fn an_error_renders_its_step_its_errno_text_and_its_entry() {
    let mut c_error = ffi::bc_file_system_error::zeroed();
    c_error.kind = ErrorKind::NoSpace as i32;
    c_error.operation = Operation::WriteFile as i32;
    c_error.step = Step::Write as i32;
    c_error.system_code = 28;
    c_error.name[..6].copy_from_slice(b"ledger");
    c_error.name_length = 6;
    c_error.name_truncated = 1;

    let error = Error::from_c(&c_error, Operation::WriteFile);

    assert_eq!(error.kind, ErrorKind::NoSpace);
    assert_eq!(error.step, Some(Step::Write));
    assert_eq!(error.name, b"ledger");
    assert!(error.name_truncated);
    let rendered = error.to_string();
    assert!(
        rendered.starts_with("write_file: no_space at write: "),
        "{rendered}"
    );
    assert!(rendered.contains("(os error 28)"), "{rendered}");
    assert!(rendered.ends_with(" (entry ledger…)"), "{rendered}");
}

#[test]
fn an_unknown_value_from_the_core_is_reported_never_guessed_silently() {
    let mut c_error = ffi::bc_file_system_error::zeroed();
    c_error.kind = 99;
    c_error.operation = 98;
    c_error.step = 97;

    let error = Error::from_c(&c_error, Operation::ReadFile);

    assert_eq!(
        error.kind,
        ErrorKind::InvalidArgument,
        "the stand-in for an unknown kind"
    );
    assert_eq!(
        error.operation,
        Operation::ReadFile,
        "the stand-in is the operation that was called"
    );
    assert_eq!(error.step, None);
    assert_eq!(
        error.unknown_value,
        Some(UnknownValue {
            enumeration: "bc_file_system_error_kind",
            value: 99
        })
    );
    assert!(
        error
            .to_string()
            .ends_with("[the core answered 99 is no bc_file_system_error_kind]")
    );
}
