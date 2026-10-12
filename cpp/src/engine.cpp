#include <bc_file_system/engine.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/error.hpp>
#include <bc_file_system/result.hpp>

// The one C++ file that names the core's factories: `live()` is the live seam's body, and
// `observed_for_tests()` and `portable_only_for_tests()` are the test-only factories the house-rules guard
// confines to test code.

namespace bc::file_system {

namespace {

// A call into the core that took no location: the error's path is empty.
constexpr std::string_view no_location;

auto create(Operation called, std::int32_t (*factory)(bc_file_system_engine**, bc_file_system_error*),
            bc_file_system_engine** handle) -> std::optional<Error> {
    bc_file_system_error c_error;
    if (factory(handle, &c_error) != 0) {
        return error_from(c_error, called, no_location);
    }
    return std::nullopt;
}

} // namespace

auto to_c(const Fault& fault) noexcept -> bc_file_system_fault {
    bc_file_system_fault c_fault;
    c_fault.operation = static_cast<std::int32_t>(fault.operation);
    c_fault.step = static_cast<std::int32_t>(fault.step);
    c_fault.occurrence = fault.occurrence;
    c_fault.action = static_cast<std::int32_t>(fault.action);
    c_fault.system_code = fault.system_code;
    return c_fault;
}

auto fault_from(const bc_file_system_fault& c_fault) noexcept -> std::optional<Fault> {
    const std::optional<Operation> operation = operation_from(c_fault.operation);
    const std::optional<Step> step = step_from(c_fault.step);
    const bool action_in_enum =
        c_fault.action >= bc_file_system_fault_action_fail && c_fault.action <= bc_file_system_fault_action_stop;
    if (!operation.has_value() || !step.has_value() || !action_in_enum) {
        return std::nullopt;
    }
    return Fault{.operation = *operation,
                 .step = *step,
                 .occurrence = c_fault.occurrence,
                 .action = static_cast<FaultAction>(c_fault.action),
                 .system_code = c_fault.system_code};
}

auto Engine::live() -> Result<Engine> {
    bc_file_system_engine* handle = nullptr;
    const std::optional<Error> failure =
        create(Operation::engine_create_live, bc_file_system_engine_create_live, &handle); // reach-ok: the live seam
    if (failure.has_value()) {
        return *failure;
    }
    return Engine{handle};
}

auto Engine::observed_for_tests() -> Result<Engine> {
    bc_file_system_engine* handle = nullptr;
    const std::optional<Error> failure =
        create(Operation::engine_create_observed_for_tests, bc_file_system_engine_create_observed_for_tests, &handle);
    if (failure.has_value()) {
        return *failure;
    }
    return Engine{handle};
}

auto Engine::portable_only_for_tests() -> Result<Engine> {
    bc_file_system_engine* handle = nullptr;
    const std::optional<Error> failure = create(Operation::engine_create_portable_only_for_tests,
                                                bc_file_system_engine_create_portable_only_for_tests, &handle);
    if (failure.has_value()) {
        return *failure;
    }
    return Engine{handle};
}

Engine::Engine(bc_file_system_engine* handle) noexcept : handle_(handle) {}

Engine::Engine(Engine&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

auto Engine::operator=(Engine&& other) noexcept -> Engine& {
    if (this != &other) {
        bc_file_system_engine_release(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Engine::~Engine() {
    bc_file_system_engine_release(handle_);
}

auto Engine::add_fault(const Fault& fault) -> Result<void> {
    const bc_file_system_fault c_fault = to_c(fault);
    bc_file_system_error c_error;
    if (bc_file_system_observed_fault_add(handle_, &c_fault, &c_error) != 0) {
        return error_from(c_error, Operation::observed_fault_add, no_location);
    }
    return Result<void>{};
}

auto Engine::clear_faults() -> Result<void> {
    bc_file_system_error c_error;
    if (bc_file_system_observed_faults_clear(handle_, &c_error) != 0) {
        return error_from(c_error, Operation::observed_faults_clear, no_location);
    }
    return Result<void>{};
}

auto Engine::step_count(Operation operation, Step step) const -> Result<std::uint64_t> {
    std::uint64_t count = 0;
    bc_file_system_error c_error;
    if (bc_file_system_observed_step_count(handle_, static_cast<std::int32_t>(operation),
                                           static_cast<std::int32_t>(step), &count, &c_error) != 0) {
        return error_from(c_error, Operation::observed_step_count, no_location);
    }
    return count;
}

} // namespace bc::file_system
