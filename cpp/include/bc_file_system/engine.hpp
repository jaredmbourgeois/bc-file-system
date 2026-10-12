#pragma once

#include <cstdint>
#include <optional>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/error.hpp>
#include <bc_file_system/result.hpp>

namespace bc::file_system {

/// What a scripted fault does when its step is reached.
enum class FaultAction : std::int32_t {
    /// The step fails with the fault's `system_code`, exactly as if the system had failed the call.
    fail = 1,
    /// The operation returns from the step without any cleanup, as if the process had been killed there: how
    /// every multi-step operation's kill points are tested.
    stop = 2,
};

static_assert(static_cast<std::int32_t>(FaultAction::fail) == bc_file_system_fault_action_fail);
static_assert(static_cast<std::int32_t>(FaultAction::stop) == bc_file_system_fault_action_stop);

/// One scripted fault: the `occurrence`-th time `operation` reaches `step`, `action` happens.
struct Fault {
    Operation operation;
    Step step;
    /// Counted from 1 across the engine's lifetime.
    std::uint32_t occurrence;
    FaultAction action;
    /// The errno to answer under `fail` (at least 1); 0 under `stop`.
    int system_code;

    [[nodiscard]] friend auto operator==(const Fault&, const Fault&) -> bool = default;
};

[[nodiscard]] auto to_c(const Fault& fault) noexcept -> bc_file_system_fault;
[[nodiscard]] auto fault_from(const bc_file_system_fault& c_fault) noexcept -> std::optional<Fault>;

/// The context every operation runs in: the platform's probed fast paths and, for tests, the observer. Created
/// by `live()` at a composition root, or by one of the two factories for tests, handed down as `Engine&`,
/// released when it goes out of scope. Move-only over the C handle.
///
/// Naming `live()` is itself a reach: it is legal only at a composition root. `observed_for_tests()` and
/// `portable_only_for_tests()` are the only ways to install the observer, and the house-rules guard confines
/// their callers to test code.
class Engine {
public:
    /// The live engine of a composition root.
    [[nodiscard]] static auto live() -> Result<Engine>;

    /// The same engine with the observer installed: every step is counted and scripted faults take effect.
    [[nodiscard]] static auto observed_for_tests() -> Result<Engine>;

    /// The observed engine with every fast path refused: every operation takes the path every platform has
    /// (the walk, the read-and-write loop, fsync), and one whose promise a fast path alone keeps (the no-replace
    /// rename of a `refuse` and of a condemn) answers `unsupported`, as a platform without that path does. For
    /// running a suite a second time, to prove the fallbacks on a platform that has every fast path.
    [[nodiscard]] static auto portable_only_for_tests() -> Result<Engine>;

    Engine(Engine&& other) noexcept;
    auto operator=(Engine&& other) noexcept -> Engine&;
    Engine(const Engine&) = delete;
    auto operator=(const Engine&) = delete;
    ~Engine();

    /// Scripts one fault into an observed engine. A live engine answers `invalid_argument`; a full slot
    /// `resource_exhausted`.
    [[nodiscard]] auto add_fault(const Fault& fault) -> Result<void>;

    /// Forgets every scripted fault; the step counts are kept.
    [[nodiscard]] auto clear_faults() -> Result<void>;

    /// How many times `operation` has reached `step` in this engine's lifetime.
    [[nodiscard]] auto step_count(Operation operation, Step step) const -> Result<std::uint64_t>;

    /// The C handle, for the face's calls. Never null while the object lives.
    [[nodiscard]] auto handle() const noexcept -> bc_file_system_engine* { return handle_; }

private:
    explicit Engine(bc_file_system_engine* handle) noexcept;

    bc_file_system_engine* handle_;
};

} // namespace bc::file_system
