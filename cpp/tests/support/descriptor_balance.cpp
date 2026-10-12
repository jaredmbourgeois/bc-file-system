#include "descriptor_balance.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#endif

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The open-descriptor sweep the fixture takes before and after every case (AGENTS.md, "Inject, never reach",
// legal site 3). Each platform has one way of listing the set; the three bodies are whole alternatives.

namespace bc::file_system_test {

namespace {

#if defined(__APPLE__)

// The kernel lists the descriptors itself, so the shell's soft limit (a million on this platform) is never
// swept.
auto list_open_descriptors() -> std::vector<int> {
    const pid_t pid = ::getpid();
    const int bytes_needed = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes_needed <= 0) {
        ADD_FAILURE() << "proc_pidinfo could not size the descriptor list";
        return {};
    }
    // Room for a few descriptors opened between the sizing call and the listing call.
    std::vector<proc_fdinfo> entries(static_cast<std::size_t>(bytes_needed) / sizeof(proc_fdinfo) + 8);
    const int bytes_filled =
        ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, entries.data(), static_cast<int>(entries.size() * sizeof(proc_fdinfo)));
    if (bytes_filled < 0) {
        ADD_FAILURE() << "proc_pidinfo could not list the descriptors";
        return {};
    }
    const std::size_t count = static_cast<std::size_t>(bytes_filled) / sizeof(proc_fdinfo);
    std::vector<int> descriptors;
    descriptors.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        descriptors.push_back(entries[index].proc_fd);
    }
    return descriptors;
}

auto description_of(int descriptor) -> std::string {
    char path[PATH_MAX] = {};
    if (::fcntl(descriptor, F_GETPATH, path) == -1) {
        return {};
    }
    return std::string{static_cast<const char*>(path)};
}

#elif defined(__linux__)

// /proc/self/fd names every open descriptor; the stream reading it holds one of its own, which is left out.
auto list_open_descriptors() -> std::vector<int> {
    DIR* stream = ::opendir("/proc/self/fd");
    if (stream == nullptr) {
        ADD_FAILURE() << "/proc/self/fd could not be opened";
        return {};
    }
    const int own_descriptor = ::dirfd(stream);
    std::vector<int> descriptors;
    for (const dirent* entry = ::readdir(stream); entry != nullptr; entry = ::readdir(stream)) {
        const std::string_view name{static_cast<const char*>(entry->d_name)};
        int descriptor = 0;
        const std::from_chars_result parsed = std::from_chars(name.data(), name.data() + name.size(), descriptor);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || descriptor == own_descriptor) {
            continue;
        }
        descriptors.push_back(descriptor);
    }
    ::closedir(stream);
    return descriptors;
}

auto description_of(int descriptor) -> std::string {
    const std::string link = "/proc/self/fd/" + std::to_string(descriptor);
    char target[PATH_MAX] = {};
    const ssize_t length = ::readlink(link.c_str(), target, sizeof(target) - 1);
    if (length < 0) {
        return {};
    }
    return std::string{static_cast<const char*>(target), static_cast<std::size_t>(length)};
}

#else

// No listing from the system: every descriptor number up to the soft limit is asked whether it is open.
auto list_open_descriptors() -> std::vector<int> {
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        ADD_FAILURE() << "getrlimit could not report the descriptor limit";
        return {};
    }
    const int highest = static_cast<int>(std::min<rlim_t>(limit.rlim_cur, static_cast<rlim_t>(INT_MAX)));
    std::vector<int> descriptors;
    for (int descriptor = 0; descriptor < highest; ++descriptor) {
        if (::fcntl(descriptor, F_GETFD) != -1) {
            descriptors.push_back(descriptor);
        }
    }
    return descriptors;
}

auto description_of(int) -> std::string {
    return {};
}

#endif

auto render(const std::vector<int>& descriptors) -> std::string {
    std::string text;
    for (const int descriptor : descriptors) {
        if (!text.empty()) {
            text += ", ";
        }
        text += std::to_string(descriptor);
        const std::string description = description_of(descriptor);
        if (!description.empty()) {
            text += " (" + description + ")";
        }
    }
    return text;
}

} // namespace

auto open_descriptors() -> std::vector<int> {
    std::vector<int> descriptors = list_open_descriptors();
    std::ranges::sort(descriptors);
    return descriptors;
}

DescriptorBalance::DescriptorBalance() : before_(open_descriptors()) {}

DescriptorBalance::~DescriptorBalance() {
    const std::vector<int> after = open_descriptors();
    std::vector<int> leaked;
    std::ranges::set_difference(after, before_, std::back_inserter(leaked));
    std::vector<int> vanished;
    std::ranges::set_difference(before_, after, std::back_inserter(vanished));
    if (!leaked.empty()) {
        ADD_FAILURE() << "the case left " << leaked.size() << " descriptor(s) open: " << render(leaked);
    }
    if (!vanished.empty()) {
        ADD_FAILURE() << "the case closed " << vanished.size()
                      << " descriptor(s) it did not open: " << render(vanished);
    }
}

} // namespace bc::file_system_test
