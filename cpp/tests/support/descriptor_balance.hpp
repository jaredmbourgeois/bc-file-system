#pragma once

#include <gtest/gtest.h>

#include <vector>

// The descriptor-balance fixture of the design doc's cleanup idiom: a leaked descriptor is the one leak no
// sanitizer reports, so the open-descriptor set is snapshotted before each case and compared after it, and a
// difference fails the case. Every suite derives its fixture from one of the two bases below.

namespace bc::file_system_test {

/// The descriptors this process holds open right now, ascending: `/proc/self/fd` on Linux, the kernel's
/// descriptor list on Apple platforms, an F_GETFD sweep up to the soft limit elsewhere.
[[nodiscard]] auto open_descriptors() -> std::vector<int>;

/// Takes the snapshot when it is constructed and compares when it is destroyed, recording a test failure that
/// names each descriptor that leaked (and, where the system can say, what it is open on) or vanished. Held as
/// the first member of a fixture, so it is constructed before and destroyed after everything else the case
/// owns, the case's temporary directory included.
class DescriptorBalance {
public:
    DescriptorBalance();
    ~DescriptorBalance();

    DescriptorBalance(const DescriptorBalance&) = delete;
    DescriptorBalance(DescriptorBalance&&) = delete;
    auto operator=(const DescriptorBalance&) -> DescriptorBalance& = delete;
    auto operator=(DescriptorBalance&&) -> DescriptorBalance& = delete;

private:
    std::vector<int> before_;
};

/// The base of every TEST_F fixture.
class DescriptorBalanced : public testing::Test {
private:
    DescriptorBalance balance_;
};

/// The base of every TEST_P fixture.
template <class Parameter>
class DescriptorBalancedWithParam : public testing::TestWithParam<Parameter> {
private:
    DescriptorBalance balance_;
};

} // namespace bc::file_system_test
