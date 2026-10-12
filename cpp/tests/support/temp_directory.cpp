#include "temp_directory.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The tests' own file system (AGENTS.md, "Inject, never reach", legal site 3): every reach here is for a
// directory the test created under the root it was handed.

namespace bc::file_system_test {

namespace {

// Gives the owner back full access to `path` and everything below it, so that a directory a test locked
// (mode 0000, 0300) can be emptied and removed.
auto restore_access(const std::filesystem::path& path) -> void {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return;
    }
    ::chmod(path.c_str(), S_IRWXU);
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{path, error}) {
        restore_access(entry.path());
    }
}

} // namespace

TemporaryDirectory::TemporaryDirectory(std::filesystem::path path) noexcept : path_(std::move(path)) {}

TemporaryDirectory::TemporaryDirectory(TemporaryDirectory&& other) noexcept
    : path_(std::exchange(other.path_, std::filesystem::path{})) {}

TemporaryDirectory::~TemporaryDirectory() {
    if (path_.empty()) {
        return;
    }
    restore_access(path_);
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

auto TemporaryDirectory::create_under(std::string_view root) -> std::optional<TemporaryDirectory> {
    std::string name = (std::filesystem::path{root} / "case-XXXXXX").string();
    if (::mkdtemp(name.data()) == nullptr) {
        ADD_FAILURE() << "mkdtemp under " << root << ": " << std::strerror(errno);
        return std::nullopt;
    }
    // Resolved once, so that the paths a test builds from it name no symlink (macOS reaches its temporary
    // directories through one) and compare equal to what the system reports.
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical(name, error);
    if (error) {
        ADD_FAILURE() << "canonical " << name << ": " << error.message();
        return std::nullopt;
    }
    return TemporaryDirectory{resolved};
}

auto TemporaryDirectory::path() const noexcept -> const std::filesystem::path& {
    return path_;
}

auto write_raw(const std::filesystem::path& path, std::string_view text, std::uint32_t mode) -> void {
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            ADD_FAILURE() << "could not write " << path;
            return;
        }
    }
    set_mode_raw(path, mode);
}

auto read_raw(const std::filesystem::path& path) -> std::string {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        ADD_FAILURE() << "could not open " << path;
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

auto make_directory_raw(const std::filesystem::path& path, std::uint32_t mode) -> void {
    if (::mkdir(path.c_str(), S_IRWXU) != 0) {
        ADD_FAILURE() << "mkdir " << path << ": " << std::strerror(errno);
        return;
    }
    set_mode_raw(path, mode);
}

auto make_symlink_raw(const std::filesystem::path& target, const std::filesystem::path& link) -> void {
    if (::symlink(target.c_str(), link.c_str()) != 0) {
        ADD_FAILURE() << "symlink " << link << " -> " << target << ": " << std::strerror(errno);
    }
}

auto make_fifo_raw(const std::filesystem::path& path, std::uint32_t mode) -> void {
    if (::mkfifo(path.c_str(), S_IRWXU) != 0) {
        ADD_FAILURE() << "mkfifo " << path << ": " << std::strerror(errno);
        return;
    }
    set_mode_raw(path, mode);
}

auto set_mode_raw(const std::filesystem::path& path, std::uint32_t mode) -> void {
    if (::chmod(path.c_str(), static_cast<mode_t>(mode)) != 0) {
        ADD_FAILURE() << "chmod " << path << ": " << std::strerror(errno);
    }
}

auto mode_raw(const std::filesystem::path& path) -> std::uint32_t {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        ADD_FAILURE() << "lstat " << path << ": " << std::strerror(errno);
        return 0;
    }
    return static_cast<std::uint32_t>(info.st_mode) & 07777U;
}

auto identity_raw(const std::filesystem::path& path) -> Identity {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        ADD_FAILURE() << "lstat " << path << ": " << std::strerror(errno);
        return Identity{.device = 0, .inode = 0};
    }
    return Identity{.device = static_cast<std::uint64_t>(info.st_dev),
                    .inode = static_cast<std::uint64_t>(info.st_ino)};
}

auto exists_raw(const std::filesystem::path& path) -> bool {
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0;
}

auto names_raw(const std::filesystem::path& directory) -> std::vector<std::string> {
    std::vector<std::string> names;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{directory, error}) {
        names.push_back(entry.path().filename().string());
    }
    if (error) {
        ADD_FAILURE() << "could not list " << directory << ": " << error.message();
    }
    std::ranges::sort(names);
    return names;
}

auto permissions_are_enforced(const std::filesystem::path& scratch) -> bool {
    const std::filesystem::path probe = scratch / ".permission-probe";
    write_raw(probe, "probe", 0000);
    const int descriptor = ::open(probe.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor >= 0) {
        ::close(descriptor);
    }
    ::unlink(probe.c_str());
    return descriptor < 0;
}

FileSizeLimit::FileSizeLimit(std::uint64_t byte_limit) : previous_limit_(0), applied_(false) {
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) {
        ADD_FAILURE() << "getrlimit: " << std::strerror(errno);
        return;
    }
    previous_limit_ = static_cast<std::uint64_t>(limit.rlim_cur);

    // Crossing the limit also raises SIGXFSZ, whose default action ends the process. Ignored, the write
    // simply fails with EFBIG, which is the failure the test is after. It stays ignored: a test binary has
    // no use for the signal, and the library never installs handling of its own (design doc, "Errors").
    std::signal(SIGXFSZ, SIG_IGN);

    limit.rlim_cur = static_cast<rlim_t>(byte_limit);
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        ADD_FAILURE() << "setrlimit: " << std::strerror(errno);
        return;
    }
    applied_ = true;
}

FileSizeLimit::~FileSizeLimit() {
    if (!applied_) {
        return;
    }
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_FSIZE, &limit) == 0) {
        limit.rlim_cur = static_cast<rlim_t>(previous_limit_);
        ::setrlimit(RLIMIT_FSIZE, &limit);
    }
}

auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char character : text) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

auto text_of(std::span<const std::byte> bytes) -> std::string {
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte byte : bytes) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

} // namespace bc::file_system_test
