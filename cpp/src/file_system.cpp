#include <bc_file_system/file_system.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/engine.hpp>
#include <bc_file_system/error.hpp>
#include <bc_file_system/location.hpp>
#include <bc_file_system/options.hpp>
#include <bc_file_system/result.hpp>
#include <bc_file_system/status.hpp>

// The C++ face: the path value types, the two handles, the location, and one method per operation, each a
// call into the core plus the conversion of its answer. The face holds no logic about the file system; what
// it owns is the buffer protocol's grow loop, the composition of error paths and the mapping of integers to
// enums.

namespace bc::file_system {

namespace {

// The first capacity offered to the core for an answer of variable length. One read serves most files and
// names outright; a larger answer costs one more call, since the core names the capacity it needs.
constexpr std::size_t first_read_capacity = std::size_t{64} * 1024;
constexpr std::size_t first_text_capacity = 1024;
constexpr std::size_t first_entry_capacity = 64;
// The grow loop's bound, so a file that grows between two calls cannot livelock the face: after this many
// attempts the `buffer_too_small` is returned as received.
constexpr int most_attempts = 3;

auto bytes_of(std::string_view text) noexcept -> const std::uint8_t* {
    return reinterpret_cast<const std::uint8_t*>(text.data());
}

auto bytes_of(std::span<const std::byte> bytes) noexcept -> const std::uint8_t* {
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

// The core answered with a value outside its own enum, which a core and a face built together cannot do (the
// pins hold them equal). Reported as a failure rather than trusted or aborted on.
auto answer_outside_enum(Operation called, std::string_view location_text) -> Error {
    return Error{.kind = ErrorKind::io_failed,
                 .operation = called,
                 .step = std::nullopt,
                 .system_code = 0,
                 .path = std::string(location_text),
                 .name_truncated = false,
                 .failed_location = std::nullopt};
}

// The buffer protocol as one loop. `call(buffer, capacity, &length, &c_error)` is the core's function with
// everything but the buffer already bound; the answer lands in a `Container` of bytes or text, sized to what
// the core stored.
template <class Container, class Call>
auto answered_into(std::size_t first_capacity, Operation called, std::string_view location_text, Call call)
    -> Result<Container> {
    Container answer(first_capacity, typename Container::value_type{});
    for (int attempt = 1;; ++attempt) {
        std::size_t length = 0;
        bc_file_system_error c_error;
        const std::int32_t kind =
            call(reinterpret_cast<std::uint8_t*>(answer.data()), answer.size(), &length, &c_error);
        if (kind == 0) {
            answer.resize(length);
            return answer;
        }
        if (kind == bc_file_system_error_kind_buffer_too_small && attempt < most_attempts) {
            answer.resize(length);
            continue;
        }
        return error_from(c_error, called, location_text);
    }
}

auto durability_answer(std::int32_t achieved, Operation called, std::string_view location_text) -> Result<Durability> {
    const std::optional<Durability> durability = durability_from(achieved);
    if (!durability.has_value()) {
        return answer_outside_enum(called, location_text);
    }
    return *durability;
}

} // namespace

// ---- Path values ---------------------------------------------------------------------------------------------

// The rules live in the path module; the value type holds what the core accepted, and a refusal is the core's
// error with the bytes it refused as its path.

auto RelativePath::from(std::string_view bytes) -> Result<RelativePath> {
    bc_file_system_error c_error;
    if (bc_file_system_relative_path_validate(bytes_of(bytes), bytes.size(), &c_error) != 0) {
        return error_from(c_error, Operation::relative_path_from, bytes);
    }
    return RelativePath{std::string(bytes)};
}

auto AbsolutePath::from(std::string_view bytes) -> Result<AbsolutePath> {
    bc_file_system_error c_error;
    if (bc_file_system_absolute_path_validate(bytes_of(bytes), bytes.size(), &c_error) != 0) {
        return error_from(c_error, Operation::absolute_path_from, bytes);
    }
    return AbsolutePath{std::string(bytes)};
}

// ---- Directory and Lock --------------------------------------------------------------------------------------

Directory::Directory(bc_file_system_directory* handle, std::string opened_as_text) noexcept
    : handle_(handle), opened_as_text_(std::move(opened_as_text)) {}

Directory::Directory(Directory&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), opened_as_text_(std::move(other.opened_as_text_)) {}

auto Directory::operator=(Directory&& other) noexcept -> Directory& {
    if (this != &other) {
        bc_file_system_directory_release(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
        opened_as_text_ = std::move(other.opened_as_text_);
    }
    return *this;
}

Directory::~Directory() {
    bc_file_system_directory_release(handle_);
}

auto Directory::opened_as() const -> Result<std::string> {
    return answered_into<std::string>(
        first_text_capacity, Operation::directory_opened_as, opened_as_text_,
        [this](std::uint8_t* buffer, std::size_t capacity, std::size_t* length, bc_file_system_error* c_error) {
            return bc_file_system_directory_opened_as(handle_, buffer, capacity, length, c_error);
        });
}

Lock::Lock(bc_file_system_lock* handle) noexcept : handle_(handle) {}

Lock::Lock(Lock&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

auto Lock::operator=(Lock&& other) noexcept -> Lock& {
    if (this != &other) {
        bc_file_system_lock_release(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Lock::~Lock() {
    bc_file_system_lock_release(handle_);
}

// ---- Location ------------------------------------------------------------------------------------------------

Location::Location(LocationKind kind, const Directory* directory, std::string bytes) noexcept
    : kind_(kind), directory_(directory), bytes_(std::move(bytes)) {}

auto Location::at(const std::filesystem::path& path) -> Location {
    return Location{LocationKind::at, nullptr, path.native()};
}

auto Location::at(const AbsolutePath& path) -> Location {
    return Location{LocationKind::at, nullptr, std::string(path.bytes())};
}

auto Location::beneath(const Directory& directory, const RelativePath& relative) -> Location {
    return Location{LocationKind::beneath, &directory, std::string(relative.bytes())};
}

auto Location::text() const -> std::string {
    if (kind_ == LocationKind::at) {
        return bytes_;
    }
    std::string text = directory_->opened_as_text_;
    text += '/';
    text += bytes_;
    return text;
}

auto Location::to_c() const noexcept -> bc_file_system_location {
    if (kind_ == LocationKind::at) {
        return bc_file_system_location_at(bytes_of(bytes_), bytes_.size());
    }
    return bc_file_system_location_beneath(directory_->handle(), bytes_of(bytes_), bytes_.size());
}

// ---- Lines ---------------------------------------------------------------------------------------------------

auto lines_of(std::span<const std::byte> buffer) -> std::vector<std::string_view> {
    const std::string_view text(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// ---- FileSystem ----------------------------------------------------------------------------------------------

FileSystem::FileSystem(Engine& engine) noexcept : engine_(engine) {}

auto FileSystem::open_directory(const Location& location, FinalSymlink final_symlink) noexcept -> Result<Directory> {
    const bc_file_system_location c_location = location.to_c();
    bc_file_system_directory* handle = nullptr;
    bc_file_system_error c_error;
    if (bc_file_system_directory_open(engine_.handle(), &c_location, static_cast<std::int32_t>(final_symlink), &handle,
                                      &c_error) != 0) {
        return error_from(c_error, Operation::open_directory, location.text());
    }
    return Directory{handle, location.text()};
}

auto FileSystem::status(const Location& location, FinalSymlink final_symlink) noexcept
    -> Result<std::optional<Status>> {
    const bc_file_system_location c_location = location.to_c();
    std::int32_t present = 0;
    bc_file_system_status c_status;
    bc_file_system_error c_error;
    if (bc_file_system_status_of(engine_.handle(), &c_location, static_cast<std::int32_t>(final_symlink), &present,
                                 &c_status, &c_error) != 0) {
        return error_from(c_error, Operation::status, location.text());
    }
    if (present == 0) {
        return std::optional<Status>{};
    }
    const std::optional<Status> answer = status_from(c_status);
    if (!answer.has_value()) {
        return answer_outside_enum(Operation::status, location.text());
    }
    return answer;
}

auto FileSystem::read_file(const Location& location, const ReadOptions& options) noexcept
    -> Result<std::vector<std::byte>> {
    const bc_file_system_location c_location = location.to_c();
    const bc_file_system_read_options c_options = to_c(options);
    // Under a bound no successful answer is longer than the bound, so a smaller bound is offered whole.
    const ReadLimit limit = options.limit.value;
    const std::size_t first_capacity =
        limit.kind() == ReadLimitKind::at_most
            ? static_cast<std::size_t>(std::min<std::uint64_t>(limit.bound(), first_read_capacity))
            : first_read_capacity;
    return answered_into<std::vector<std::byte>>(
        first_capacity, Operation::read_file, location.text(),
        [this, &c_location, &c_options](std::uint8_t* buffer, std::size_t capacity, std::size_t* length,
                                        bc_file_system_error* c_error) {
            return bc_file_system_read_file(engine_.handle(), &c_location, &c_options, buffer, capacity, length,
                                            c_error);
        });
}

auto FileSystem::read_file_if_present(const Location& location, const ReadOptions& options) noexcept
    -> Result<std::optional<std::vector<std::byte>>> {
    Result<std::vector<std::byte>> answer = read_file(location, options);
    if (answer.has_value()) {
        return std::optional<std::vector<std::byte>>{std::move(answer).value()};
    }
    if (answer.error().kind == ErrorKind::not_found) {
        return std::optional<std::vector<std::byte>>{};
    }
    return std::move(answer).error();
}

auto FileSystem::write_file(const Location& location, std::span<const std::byte> bytes,
                            const WriteOptions& options) noexcept -> Result<Durability> {
    const bc_file_system_location c_location = location.to_c();
    const bc_file_system_write_options c_options = to_c(options);
    std::int32_t achieved = 0;
    bc_file_system_error c_error;
    if (bc_file_system_write_file(engine_.handle(), &c_location, bytes_of(bytes), bytes.size(), &c_options, &achieved,
                                  &c_error) != 0) {
        return error_from(c_error, Operation::write_file, location.text());
    }
    return durability_answer(achieved, Operation::write_file, location.text());
}

auto FileSystem::append(const Location& location, std::span<const std::byte> bytes, Durability durability,
                        Permissions permissions) noexcept -> Result<Durability> {
    const bc_file_system_location c_location = location.to_c();
    const bc_file_system_permissions c_permissions = to_c(permissions);
    std::int32_t achieved = 0;
    bc_file_system_error c_error;
    if (bc_file_system_append(engine_.handle(), &c_location, bytes_of(bytes), bytes.size(),
                              static_cast<std::int32_t>(durability), &c_permissions, &achieved, &c_error) != 0) {
        return error_from(c_error, Operation::append, location.text());
    }
    return durability_answer(achieved, Operation::append, location.text());
}

auto FileSystem::sync(const Location& location) noexcept -> Result<Durability> {
    const bc_file_system_location c_location = location.to_c();
    std::int32_t achieved = 0;
    bc_file_system_error c_error;
    if (bc_file_system_sync(engine_.handle(), &c_location, &achieved, &c_error) != 0) {
        return error_from(c_error, Operation::sync, location.text());
    }
    return durability_answer(achieved, Operation::sync, location.text());
}

auto FileSystem::create_directory(const Location& location, Permissions permissions) noexcept -> Result<void> {
    const bc_file_system_location c_location = location.to_c();
    const bc_file_system_permissions c_permissions = to_c(permissions);
    bc_file_system_error c_error;
    if (bc_file_system_create_directory(engine_.handle(), &c_location, &c_permissions, &c_error) != 0) {
        return error_from(c_error, Operation::create_directory, location.text());
    }
    return Result<void>{};
}

auto FileSystem::create_directories(const Location& location, Permissions permissions) noexcept -> Result<void> {
    const bc_file_system_location c_location = location.to_c();
    const bc_file_system_permissions c_permissions = to_c(permissions);
    bc_file_system_error c_error;
    if (bc_file_system_create_directories(engine_.handle(), &c_location, &c_permissions, &c_error) != 0) {
        return error_from(c_error, Operation::create_directories, location.text());
    }
    return Result<void>{};
}

auto FileSystem::list_directory(const Location& location, FinalSymlink final_symlink) noexcept
    -> Result<std::vector<Entry>> {
    const bc_file_system_location c_location = location.to_c();
    // The buffer protocol over an array of entries rather than bytes: the same bounded grow loop.
    std::vector<bc_file_system_entry> c_entries(first_entry_capacity);
    for (int attempt = 1;; ++attempt) {
        std::size_t count = 0;
        bc_file_system_error c_error;
        const std::int32_t kind =
            bc_file_system_list_directory(engine_.handle(), &c_location, static_cast<std::int32_t>(final_symlink),
                                          c_entries.data(), c_entries.size(), &count, &c_error);
        if (kind == 0) {
            std::vector<Entry> entries;
            entries.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                std::optional<Entry> entry = entry_from(c_entries[index]);
                if (!entry.has_value()) {
                    return answer_outside_enum(Operation::list_directory, location.text());
                }
                entries.push_back(std::move(*entry));
            }
            return entries;
        }
        if (kind == bc_file_system_error_kind_buffer_too_small && attempt < most_attempts) {
            c_entries.resize(count);
            continue;
        }
        return error_from(c_error, Operation::list_directory, location.text());
    }
}

auto FileSystem::remove_file(const Location& location) noexcept -> Result<bool> {
    const bc_file_system_location c_location = location.to_c();
    std::int32_t existed = 0;
    bc_file_system_error c_error;
    if (bc_file_system_remove_file(engine_.handle(), &c_location, &existed, &c_error) != 0) {
        return error_from(c_error, Operation::remove_file, location.text());
    }
    return existed != 0;
}

auto FileSystem::remove_directory(const Location& location) noexcept -> Result<bool> {
    const bc_file_system_location c_location = location.to_c();
    std::int32_t existed = 0;
    bc_file_system_error c_error;
    if (bc_file_system_remove_directory(engine_.handle(), &c_location, &existed, &c_error) != 0) {
        return error_from(c_error, Operation::remove_directory, location.text());
    }
    return existed != 0;
}

auto FileSystem::remove_tree(const Location& location) noexcept -> Result<bool> {
    const bc_file_system_location c_location = location.to_c();
    std::int32_t existed = 0;
    bc_file_system_error c_error;
    if (bc_file_system_remove_tree(engine_.handle(), &c_location, &existed, &c_error) != 0) {
        return error_from(c_error, Operation::remove_tree, location.text());
    }
    return existed != 0;
}

auto FileSystem::create_temporary_directory(const Location& parent, Permissions permissions) noexcept
    -> Result<std::string> {
    const bc_file_system_location c_parent = parent.to_c();
    const bc_file_system_permissions c_permissions = to_c(permissions);
    return answered_into<std::string>(
        first_text_capacity, Operation::create_temporary_directory, parent.text(),
        [this, &c_parent, &c_permissions](std::uint8_t* buffer, std::size_t capacity, std::size_t* length,
                                          bc_file_system_error* c_error) {
            return bc_file_system_create_temporary_directory(engine_.handle(), &c_parent, &c_permissions, buffer,
                                                             capacity, length, c_error);
        });
}

auto FileSystem::rename(const Location& source, const Location& destination, ExistingDestination existing_destination,
                        Durability durability) noexcept -> Result<Durability> {
    const bc_file_system_location c_source = source.to_c();
    const bc_file_system_location c_destination = destination.to_c();
    std::int32_t achieved = 0;
    bc_file_system_error c_error;
    if (bc_file_system_rename(engine_.handle(), &c_source, &c_destination,
                              static_cast<std::int32_t>(existing_destination), static_cast<std::int32_t>(durability),
                              &achieved, &c_error) != 0) {
        return error_from_two_locations(c_error, Operation::rename, source.text(), destination.text());
    }
    return durability_answer(achieved, Operation::rename, destination.text());
}

auto FileSystem::copy_file(const Location& source, const Location& destination, const WriteOptions& options) noexcept
    -> Result<Durability> {
    const bc_file_system_location c_source = source.to_c();
    const bc_file_system_location c_destination = destination.to_c();
    const bc_file_system_write_options c_options = to_c(options);
    std::int32_t achieved = 0;
    bc_file_system_error c_error;
    if (bc_file_system_copy_file(engine_.handle(), &c_source, &c_destination, &c_options, &achieved, &c_error) != 0) {
        return error_from_two_locations(c_error, Operation::copy_file, source.text(), destination.text());
    }
    return durability_answer(achieved, Operation::copy_file, destination.text());
}

auto FileSystem::set_permissions(const Location& location, std::uint32_t mode, FinalSymlink final_symlink) noexcept
    -> Result<void> {
    const bc_file_system_location c_location = location.to_c();
    bc_file_system_error c_error;
    if (bc_file_system_set_permissions(engine_.handle(), &c_location, mode, static_cast<std::int32_t>(final_symlink),
                                       &c_error) != 0) {
        return error_from(c_error, Operation::set_permissions, location.text());
    }
    return Result<void>{};
}

auto FileSystem::create_symlink(std::string_view target, const Location& link) noexcept -> Result<void> {
    const bc_file_system_location c_link = link.to_c();
    bc_file_system_error c_error;
    if (bc_file_system_create_symlink(engine_.handle(), bytes_of(target), target.size(), &c_link, &c_error) != 0) {
        return error_from(c_error, Operation::create_symlink, link.text());
    }
    return Result<void>{};
}

auto FileSystem::read_symlink(const Location& location) noexcept -> Result<std::string> {
    const bc_file_system_location c_location = location.to_c();
    return answered_into<std::string>(first_text_capacity, Operation::read_symlink, location.text(),
                                      [this, &c_location](std::uint8_t* buffer, std::size_t capacity,
                                                          std::size_t* length, bc_file_system_error* c_error) {
                                          return bc_file_system_read_symlink(engine_.handle(), &c_location, buffer,
                                                                             capacity, length, c_error);
                                      });
}

auto FileSystem::try_lock(const Location& location) noexcept -> Result<std::optional<Lock>> {
    const bc_file_system_location c_location = location.to_c();
    bc_file_system_lock* handle = nullptr;
    std::int32_t c_outcome = 0;
    bc_file_system_error c_error;
    if (bc_file_system_try_lock(engine_.handle(), &c_location, &handle, &c_outcome, &c_error) != 0) {
        return error_from(c_error, Operation::try_lock, location.text());
    }
    const std::optional<LockOutcome> outcome = lock_outcome_from(c_outcome);
    if (!outcome.has_value()) {
        return answer_outside_enum(Operation::try_lock, location.text());
    }
    if (*outcome == LockOutcome::held_elsewhere) {
        return std::optional<Lock>{};
    }
    return std::optional<Lock>{Lock{handle}};
}

auto FileSystem::condemn(const Location& location) noexcept -> Result<std::string> {
    const bc_file_system_location c_location = location.to_c();
    return answered_into<std::string>(first_text_capacity, Operation::condemn, location.text(),
                                      [this, &c_location](std::uint8_t* buffer, std::size_t capacity,
                                                          std::size_t* length, bc_file_system_error* c_error) {
                                          return bc_file_system_condemn(engine_.handle(), &c_location, buffer, capacity,
                                                                        length, c_error);
                                      });
}

auto FileSystem::remove_condemned(const Location& parent) noexcept -> Result<void> {
    const bc_file_system_location c_parent = parent.to_c();
    bc_file_system_error c_error;
    if (bc_file_system_remove_condemned(engine_.handle(), &c_parent, &c_error) != 0) {
        return error_from(c_error, Operation::remove_condemned, parent.text());
    }
    return Result<void>{};
}

} // namespace bc::file_system
