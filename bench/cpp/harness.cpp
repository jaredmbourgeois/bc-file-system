#include "harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// The timing and the table of the benchmark. Nothing here reaches: the clock and the stream are handed in by
// `main`, and every action a row runs was built there.

namespace bc::file_system::benchmark {

namespace {

// The summary of one form's timed samples, which are never empty (every run took one). Both answers are samples
// that were taken, never interpolated: the median is the middle one (the upper of the two middle ones for an even
// count), and the 90th percentile is the nearest rank, the sample at or below which nine tenths of them fall:
// rank ceil(0.9 × count), counted from 1.
auto summary_of(std::vector<std::int64_t> samples) -> Summary {
    std::ranges::sort(samples);
    const std::size_t count = samples.size();
    const std::size_t median_index = count / 2;
    const std::size_t p90_rank = ((count * 9) + 9) / 10;
    return Summary{.median_nanoseconds = samples[median_index], .p90_nanoseconds = samples[p90_rank - 1]};
}

} // namespace

auto measure_interleaved(Now now, const Repetitions& repetitions, const std::vector<Timed>& forms) -> GroupMeasurement {
    for (int run = 0; run < repetitions.warm_up; ++run) {
        for (std::size_t form = 0; form < forms.size(); ++form) {
            if (!forms[form].prepare() || !forms[form].action()) {
                return GroupMeasurement{.summaries = {}, .failed_form = form};
            }
        }
    }
    std::vector<std::vector<std::int64_t>> samples(forms.size());
    for (std::vector<std::int64_t>& form_samples : samples) {
        form_samples.reserve(static_cast<std::size_t>(repetitions.timed));
    }
    for (int run = 0; run < repetitions.timed; ++run) {
        for (std::size_t form = 0; form < forms.size(); ++form) {
            if (!forms[form].prepare()) {
                return GroupMeasurement{.summaries = {}, .failed_form = form};
            }
            const std::chrono::steady_clock::time_point started = now();
            const bool succeeded = forms[form].action();
            const std::chrono::steady_clock::time_point finished = now();
            if (!succeeded) {
                return GroupMeasurement{.summaries = {}, .failed_form = form};
            }
            samples[form].push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
        }
    }
    GroupMeasurement measured{.summaries = {}, .failed_form = std::nullopt};
    for (std::vector<std::int64_t>& form_samples : samples) {
        measured.summaries.push_back(summary_of(std::move(form_samples)));
    }
    return measured;
}

namespace {

// `hundredths` as "<whole>.<two digits>", the shape both columns of numbers share.
auto two_decimals(std::int64_t hundredths) -> std::string {
    const std::int64_t fraction = hundredths % 100;
    std::string text = std::to_string(hundredths / 100);
    text += '.';
    if (fraction < 10) {
        text += '0';
    }
    text += std::to_string(fraction);
    return text;
}

} // namespace

auto microseconds_text(std::int64_t nanoseconds) -> std::string {
    // A hundredth of a microsecond is ten nanoseconds; adding five rounds to the nearest one.
    return two_decimals((nanoseconds + 5) / 10);
}

auto ratio_text(std::int64_t numerator, std::int64_t denominator) -> std::string {
    if (denominator <= 0) {
        return "";
    }
    return two_decimals(((numerator * 100) + (denominator / 2)) / denominator) + "×";
}

auto print_table(std::ostream& output, const std::vector<TableRow>& rows) -> void {
    output << "| operation | case | form | guarantee | median µs | p90 µs | vs same | calls | steps |\n";
    output << "|---|---|---|---|---:|---:|---:|---:|---|\n";
    for (const TableRow& row : rows) {
        output << "| " << row.operation << " | " << row.case_label << " | " << row.form << " | " << row.guarantee
               << " | " << microseconds_text(row.summary.median_nanoseconds) << " | "
               << microseconds_text(row.summary.p90_nanoseconds) << " | " << row.versus_same << " | " << row.calls
               << " | " << row.steps << " |\n";
    }
}

} // namespace bc::file_system::benchmark
