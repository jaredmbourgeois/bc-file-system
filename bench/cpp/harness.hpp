#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace bc::file_system::benchmark {

/// The clock a measurement reads. Handed in by the composition root, so the harness names no clock itself.
using Now = std::chrono::steady_clock::time_point (*)();

/// One run of what a row measures, or of what it does before each run without being timed. Answers whether it
/// did what it should: a row whose action fails is not measured, because its numbers would time a failure.
using Action = std::function<bool()>;

/// How many times a row's action runs: first untimed, to warm the caches and the allocator, then timed.
struct Repetitions {
    int warm_up;
    int timed;
};

/// The middle sample and the 90th-percentile sample of one row's timed runs, in nanoseconds.
struct Summary {
    std::int64_t median_nanoseconds;
    std::int64_t p90_nanoseconds;
};

/// Runs `prepare` and then `action`, `warm_up` times, then `timed` times with only `action` between two readings
/// of `now`. Answers the summary of the timed samples, or nothing as soon as `prepare` or `action` fails.
[[nodiscard]] auto measure(Now now, const Repetitions& repetitions, const Action& prepare, const Action& action)
    -> std::optional<Summary>;

/// One line of the printed table. The library's rows carry the system calls the operation made, counted on an
/// observed engine; the standard library's rows leave those cells empty.
struct TableRow {
    std::string operation;
    std::string case_label;
    std::string form;
    /// "library" for the library's own rows; "same" or "less: <what it does not promise>" for a comparison row,
    /// judged against the library row of the same operation and case.
    std::string guarantee;
    Summary summary;
    /// The library's live row only: its median over the median of the comparison row with the same guarantee.
    std::string versus_same;
    /// The gated system calls one run made (every step but the identity comparison, which calls nothing).
    std::string calls;
    /// Those calls by step, in the header's step order.
    std::string steps;
};

/// Nanoseconds as microseconds with two decimals ("12.34"), by integer arithmetic so the text is exact.
[[nodiscard]] auto microseconds_text(std::int64_t nanoseconds) -> std::string;

/// `numerator / denominator` with two decimals and a multiplication sign ("1.35×"), for the versus-same column.
[[nodiscard]] auto ratio_text(std::int64_t numerator, std::int64_t denominator) -> std::string;

/// Writes `rows` as one Markdown table, the header row first.
auto print_table(std::ostream& output, const std::vector<TableRow>& rows) -> void;

} // namespace bc::file_system::benchmark
