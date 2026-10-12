#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <bc_file_system/bc_file_system.h>

namespace bc::file_system {

/// What an entry is, judged without following a symlink.
enum class EntryType : std::int32_t {
    regular_file = 1,
    directory = 2,
    /// Reported, never followed: a link resolves to whatever it points at, and the library does not go there.
    symlink = 3,
    /// A socket, a FIFO, a device.
    other = 4,
};

static_assert(static_cast<std::int32_t>(EntryType::regular_file) == bc_file_system_entry_type_regular_file);
static_assert(static_cast<std::int32_t>(EntryType::directory) == bc_file_system_entry_type_directory);
static_assert(static_cast<std::int32_t>(EntryType::symlink) == bc_file_system_entry_type_symlink);
static_assert(static_cast<std::int32_t>(EntryType::other) == bc_file_system_entry_type_other);

/// What the file system records about one entry.
struct Status {
    EntryType entry_type;
    /// A regular file's size in bytes; 0 for every other type.
    std::int64_t size;
    /// The permission bits (`0644`), without the file-type bits.
    std::uint32_t mode;
    /// st_dev and st_ino: the entry's identity, which `remove_tree` and the resolution layer compare.
    std::uint64_t device;
    std::uint64_t inode;
    std::uint64_t link_count;

    [[nodiscard]] friend auto operator==(const Status&, const Status&) -> bool = default;
};

/// One entry of a directory listing: its name, one component, and what it is.
struct Entry {
    EntryType entry_type;
    std::string name;

    [[nodiscard]] friend auto operator==(const Entry&, const Entry&) -> bool = default;
};

/// The enum a value from the C boundary names, or empty when the value is outside the enum.
[[nodiscard]] constexpr auto entry_type_from(std::int32_t value) noexcept -> std::optional<EntryType> {
    if (value >= bc_file_system_entry_type_regular_file && value <= bc_file_system_entry_type_other) {
        return static_cast<EntryType>(value);
    }
    return std::nullopt;
}

/// Each value type crosses the boundary in one function per direction. The C side's entry type is refused
/// once here when it is outside the enum, so nothing above has to.
[[nodiscard]] constexpr auto status_from(const bc_file_system_status& c_status) noexcept -> std::optional<Status> {
    const std::optional<EntryType> entry_type = entry_type_from(c_status.entry_type);
    if (!entry_type.has_value()) {
        return std::nullopt;
    }
    return Status{.entry_type = *entry_type,
                  .size = c_status.size,
                  .mode = c_status.mode,
                  .device = c_status.device,
                  .inode = c_status.inode,
                  .link_count = c_status.link_count};
}

[[nodiscard]] constexpr auto to_c(const Status& status) noexcept -> bc_file_system_status {
    bc_file_system_status c_status;
    c_status.entry_type = static_cast<std::int32_t>(status.entry_type);
    c_status.size = status.size;
    c_status.mode = status.mode;
    c_status.device = status.device;
    c_status.inode = status.inode;
    c_status.link_count = status.link_count;
    return c_status;
}

[[nodiscard]] inline auto entry_from(const bc_file_system_entry& c_entry) -> std::optional<Entry> {
    const std::optional<EntryType> entry_type = entry_type_from(c_entry.entry_type);
    if (!entry_type.has_value() || c_entry.name_length > sizeof(c_entry.name)) {
        return std::nullopt;
    }
    return Entry{.entry_type = *entry_type,
                 .name = std::string(reinterpret_cast<const char*>(c_entry.name), c_entry.name_length)};
}

} // namespace bc::file_system
