#pragma once

#include <Monarc/Core/Types.h>

#include <string_view>
#include <type_traits>

namespace Monarc {

/// 64-bit FNV-1a over a byte range. Deterministic across runs and platforms, which matters
/// because hashes reach the cook cache and asset identity later -- see ADR-0008.
/// A null pointer with a zero length is valid and hashes as the empty range.
[[nodiscard]] u64 HashBytes(const void* data, usize size);

/// Mixes an integer so that neighbouring values differ in their high bits. Sequential ids
/// are the common key in an engine, and a weak integer hash sends them to adjacent buckets,
/// which is precisely the worst case for linear probing.
[[nodiscard]] constexpr u64 HashInteger(u64 value) {
    // splitmix64's finaliser. A hand-rolled multiply-shift mixer tends to leave the high
    // bits under-mixed for small, sequential inputs; this one is a well-studied bijection
    // on the 64-bit space chosen specifically to avalanche every output bit.
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

/// Customisation point. Specialise for a user type to make it usable as a HashMap key.
template <typename T>
struct Hasher;

/// Generic entry point. Prefer this over Hasher<T>{} at call sites.
template <typename T>
[[nodiscard]] u64 Hash(const T& value) {
    return Hasher<T>{}(value);
}

// -----------------------------------------------------------------------------------------
// Built-in Hasher specialisations.
// -----------------------------------------------------------------------------------------

template <>
struct Hasher<std::string_view> {
    [[nodiscard]] u64 operator()(std::string_view value) const {
        return HashBytes(value.data(), value.size());
    }
};

template <>
struct Hasher<bool> {
    [[nodiscard]] u64 operator()(bool value) const { return HashInteger(value ? 1u : 0u); }
};

template <>
struct Hasher<u8> {
    [[nodiscard]] u64 operator()(u8 value) const { return HashInteger(value); }
};

template <>
struct Hasher<u16> {
    [[nodiscard]] u64 operator()(u16 value) const { return HashInteger(value); }
};

template <>
struct Hasher<u32> {
    [[nodiscard]] u64 operator()(u32 value) const { return HashInteger(value); }
};

template <>
struct Hasher<u64> {
    [[nodiscard]] u64 operator()(u64 value) const { return HashInteger(value); }
};

template <>
struct Hasher<i8> {
    [[nodiscard]] u64 operator()(i8 value) const {
        return HashInteger(static_cast<u64>(value));
    }
};

template <>
struct Hasher<i16> {
    [[nodiscard]] u64 operator()(i16 value) const {
        return HashInteger(static_cast<u64>(value));
    }
};

template <>
struct Hasher<i32> {
    [[nodiscard]] u64 operator()(i32 value) const {
        return HashInteger(static_cast<u64>(value));
    }
};

template <>
struct Hasher<i64> {
    [[nodiscard]] u64 operator()(i64 value) const {
        return HashInteger(static_cast<u64>(value));
    }
};

/// Covers every pointer type in one partial specialisation, hashing the address itself.
template <typename T>
struct Hasher<T*> {
    [[nodiscard]] u64 operator()(T* value) const {
        return HashInteger(reinterpret_cast<uptr>(value));
    }
};

}  // namespace Monarc
