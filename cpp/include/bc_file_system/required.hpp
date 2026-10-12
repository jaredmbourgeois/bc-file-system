#pragma once

#include <utility>

namespace bc::file_system {

/// A field its struct cannot be built without.
///
/// A plain field left out of a designated initializer is quietly given zero, which is a default by another name.
/// A field of this type has no default constructor, so leaving it out does not compile, on any compiler:
/// `ReadOptions{.limit = ReadLimit::whole_file()}` is an error until `.final_symlink` is stated too.
template <class T>
struct Required {
    // Implicit on purpose, so that a field is written `.durability = Durability::durable` and not
    // `.durability = Required<Durability>{Durability::durable}`.
    constexpr Required(T stated) : value(std::move(stated)) {}

    T value;
};

} // namespace bc::file_system
