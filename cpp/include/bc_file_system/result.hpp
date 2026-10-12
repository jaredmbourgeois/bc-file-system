#pragma once

#include <cstdlib>
#include <optional>
#include <utility>
#include <variant>

#include <bc_file_system/error.hpp>

namespace bc::file_system {

namespace detail {

/// Ends the process when a Result is asked for the outcome it does not hold. The face never throws, so a broken
/// precondition has nowhere quieter to go. (The no-abort rule is the C core's: the core returns
/// `invalid_argument`; this is the C++ side's own misuse of its own value type.)
inline auto require_outcome(bool held) noexcept -> void {
    if (!held) {
        std::abort();
    }
}

} // namespace detail

/// The outcome of an operation: its value, or the Error that prevented it.
///
/// The member names are std::expected's (`has_value`, `value`, `error`, `operator*`, `operator->`), so code written
/// against a Result reads the same against `std::expected<T, Error>` in a C++23 code base.
///
/// Nothing here throws. Asking a failure for its value, or a success for its error, is a programming error and
/// ends the process: check `has_value()` first.
template <class T>
class [[nodiscard]] Result {
public:
    // Both conversions are implicit on purpose, as std::expected's value conversion is: an operation ends in
    // `return bytes;` or `return error;`, and each line says which outcome it is.
    Result(T value) : outcome_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : outcome_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] auto has_value() const noexcept -> bool { return outcome_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] auto value() & noexcept -> T& {
        detail::require_outcome(has_value());
        return *std::get_if<0>(&outcome_);
    }
    [[nodiscard]] auto value() const& noexcept -> const T& {
        detail::require_outcome(has_value());
        return *std::get_if<0>(&outcome_);
    }
    [[nodiscard]] auto value() && noexcept -> T&& {
        detail::require_outcome(has_value());
        return std::move(*std::get_if<0>(&outcome_));
    }

    [[nodiscard]] auto error() const& noexcept -> const Error& {
        detail::require_outcome(!has_value());
        return *std::get_if<1>(&outcome_);
    }
    [[nodiscard]] auto error() && noexcept -> Error&& {
        detail::require_outcome(!has_value());
        return std::move(*std::get_if<1>(&outcome_));
    }

    [[nodiscard]] auto operator*() & noexcept -> T& { return value(); }
    [[nodiscard]] auto operator*() const& noexcept -> const T& { return value(); }
    [[nodiscard]] auto operator->() noexcept -> T* { return &value(); }
    [[nodiscard]] auto operator->() const noexcept -> const T* { return &value(); }

private:
    std::variant<T, Error> outcome_;
};

/// The outcome of an operation that returns nothing: success, or the Error that prevented it.
template <>
class [[nodiscard]] Result<void> {
public:
    /// Success.
    Result() noexcept = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] auto has_value() const noexcept -> bool { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    auto value() const noexcept -> void { detail::require_outcome(has_value()); }

    // Spelled on the optional itself rather than through `require_outcome`, so an analyzer sees the check
    // that guards the access.
    [[nodiscard]] auto error() const& noexcept -> const Error& {
        if (!error_.has_value()) {
            std::abort();
        }
        return *error_;
    }
    [[nodiscard]] auto error() && noexcept -> Error&& {
        if (!error_.has_value()) {
            std::abort();
        }
        return std::move(*error_);
    }

private:
    std::optional<Error> error_;
};

} // namespace bc::file_system
