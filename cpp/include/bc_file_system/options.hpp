#pragma once

#include <cstdint>
#include <optional>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/required.hpp>

namespace bc::file_system {

/// What to do when the last component of a path is a symlink: the link itself is the entry, or the operation
/// acts on what it points at. Required wherever both answers are in measured use.
enum class FinalSymlink : std::int32_t {
    refuse = 1,
    follow = 2,
};

static_assert(static_cast<std::int32_t>(FinalSymlink::refuse) == bc_file_system_final_symlink_refuse);
static_assert(static_cast<std::int32_t>(FinalSymlink::follow) == bc_file_system_final_symlink_follow);

/// What to do when a mutation's destination already holds something.
enum class ExistingDestination : std::int32_t {
    replace = 1,
    /// `already_exists`, and the destination is untouched.
    refuse = 2,
};

static_assert(static_cast<std::int32_t>(ExistingDestination::replace) == bc_file_system_existing_destination_replace);
static_assert(static_cast<std::int32_t>(ExistingDestination::refuse) == bc_file_system_existing_destination_refuse);

/// How far a change must travel before the call returns. Every write, append, sync, rename and copy is told
/// which one it needs and answers with the one it achieved: once a change is visible, a sync that fails lowers
/// the answer to `written` rather than raising an error, because the change happened.
enum class Durability : std::int32_t {
    /// Handed to the operating system; outlives the process; a power loss can still lose it.
    written = 1,
    /// The bytes and the name that points at them reached stable storage: `F_FULLFSYNC` on Apple platforms,
    /// `fsync` on the file and on its directory elsewhere.
    durable = 2,
};

static_assert(static_cast<std::int32_t>(Durability::written) == bc_file_system_durability_written);
static_assert(static_cast<std::int32_t>(Durability::durable) == bc_file_system_durability_durable);

/// The durability a value from the C boundary names, or empty when the value is outside the enum.
[[nodiscard]] constexpr auto durability_from(std::int32_t value) noexcept -> std::optional<Durability> {
    if (value >= bc_file_system_durability_written && value <= bc_file_system_durability_durable) {
        return static_cast<Durability>(value);
    }
    return std::nullopt;
}

enum class ReadLimitKind : std::int32_t {
    whole_file = 1,
    at_most = 2,
};

static_assert(static_cast<std::int32_t>(ReadLimitKind::whole_file) == bc_file_system_read_limit_kind_whole_file);
static_assert(static_cast<std::int32_t>(ReadLimitKind::at_most) == bc_file_system_read_limit_kind_at_most);

/// How much of a file a read may take. There is no default: a caller builds one of the two with the factory
/// that names its choice.
class ReadLimit {
public:
    /// Every byte, however many.
    [[nodiscard]] static constexpr auto whole_file() noexcept -> ReadLimit {
        return ReadLimit{ReadLimitKind::whole_file, 0};
    }

    /// At most `bytes`; a larger file is `too_large`, never a silent prefix.
    [[nodiscard]] static constexpr auto at_most(std::uint64_t bytes) noexcept -> ReadLimit {
        return ReadLimit{ReadLimitKind::at_most, bytes};
    }

    [[nodiscard]] constexpr auto kind() const noexcept -> ReadLimitKind { return kind_; }
    /// The bound under `at_most`; 0 under `whole_file`, where it means nothing.
    [[nodiscard]] constexpr auto bound() const noexcept -> std::uint64_t { return bound_; }

    [[nodiscard]] friend constexpr auto operator==(const ReadLimit&, const ReadLimit&) noexcept -> bool = default;

private:
    constexpr ReadLimit(ReadLimitKind kind, std::uint64_t bound) noexcept : kind_(kind), bound_(bound) {}

    ReadLimitKind kind_;
    std::uint64_t bound_;
};

enum class PermissionsPolicy : std::int32_t {
    exactly = 1,
    preserve_or = 2,
};

static_assert(static_cast<std::int32_t>(PermissionsPolicy::exactly) == bc_file_system_permissions_policy_exactly);
static_assert(static_cast<std::int32_t>(PermissionsPolicy::preserve_or) ==
              bc_file_system_permissions_policy_preserve_or);

/// The mode of anything an operation creates or replaces. There is no default mode: a caller builds one of the
/// two with the factory that names its policy.
///
/// `mode` is the permission bits as chmod(2) takes them (`0644`, `0600`, `0755`); a value with bits outside
/// `07777` makes the operation fail with `invalid_argument`. The mode is applied exactly: the process umask is
/// ambient state the library does not consult, so a caller that wants it masks the mode it passes.
///
/// What already exists and is not replaced is never touched: appending to an existing file leaves its mode
/// alone, and so does naming a directory that is already there.
class Permissions {
public:
    /// A created file or directory gets `mode`, and so does a file that replaces an existing one.
    [[nodiscard]] static constexpr auto exactly(std::uint32_t mode) noexcept -> Permissions {
        return Permissions{PermissionsPolicy::exactly, mode};
    }

    /// A file that replaces an existing one keeps the mode the replaced file had, so an operator's chmod
    /// survives a rewrite; anything newly created gets `mode`.
    [[nodiscard]] static constexpr auto preserve_or(std::uint32_t mode) noexcept -> Permissions {
        return Permissions{PermissionsPolicy::preserve_or, mode};
    }

    [[nodiscard]] constexpr auto policy() const noexcept -> PermissionsPolicy { return policy_; }
    [[nodiscard]] constexpr auto mode() const noexcept -> std::uint32_t { return mode_; }

    [[nodiscard]] friend constexpr auto operator==(const Permissions&, const Permissions&) noexcept -> bool = default;

private:
    constexpr Permissions(PermissionsPolicy policy, std::uint32_t mode) noexcept : policy_(policy), mode_(mode) {}

    PermissionsPolicy policy_;
    std::uint32_t mode_;
};

/// Every choice a write, a copy or a replace makes, each one required. Built once as a named value at each
/// composition root; a field added later fails to compile at every root, which is the point.
struct WriteOptions {
    Required<Durability> durability;
    Required<Permissions> permissions;
    Required<ExistingDestination> existing_destination;
    Required<FinalSymlink> final_symlink;
};

/// Every choice a read makes, each one required.
struct ReadOptions {
    Required<ReadLimit> limit;
    Required<FinalSymlink> final_symlink;
};

/// Each value type crosses to its C struct in one function, so the field order lives in one place.
[[nodiscard]] constexpr auto to_c(const ReadLimit& limit) noexcept -> bc_file_system_read_limit {
    bc_file_system_read_limit c_limit;
    c_limit.kind = static_cast<std::int32_t>(limit.kind());
    c_limit.at_most = limit.bound();
    return c_limit;
}

[[nodiscard]] constexpr auto to_c(const Permissions& permissions) noexcept -> bc_file_system_permissions {
    bc_file_system_permissions c_permissions;
    c_permissions.policy = static_cast<std::int32_t>(permissions.policy());
    c_permissions.mode = permissions.mode();
    return c_permissions;
}

[[nodiscard]] constexpr auto to_c(const WriteOptions& options) noexcept -> bc_file_system_write_options {
    bc_file_system_write_options c_options;
    c_options.durability = static_cast<std::int32_t>(options.durability.value);
    c_options.permissions = to_c(options.permissions.value);
    c_options.existing_destination = static_cast<std::int32_t>(options.existing_destination.value);
    c_options.final_symlink = static_cast<std::int32_t>(options.final_symlink.value);
    return c_options;
}

[[nodiscard]] constexpr auto to_c(const ReadOptions& options) noexcept -> bc_file_system_read_options {
    bc_file_system_read_options c_options;
    c_options.limit = to_c(options.limit.value);
    c_options.final_symlink = static_cast<std::int32_t>(options.final_symlink.value);
    return c_options;
}

} // namespace bc::file_system
