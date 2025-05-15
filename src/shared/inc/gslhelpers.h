/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    gslhelpers.h

Abstract:

    This file contains various GSL helper methods.

--*/

#pragma once

#include <gsl/span>

namespace gslhelpers {

template <class T>
constexpr bool is_standard_layout_v = std::is_standard_layout_v<T>;

namespace details {
    //
    // Helper function, do not use outside of this header.
    //
    // A performant bounds check that ensures that it is valid to read
    // access_size starting from span[offset].
    //

    template <class SpanType, class OffsetType, class AccessSizeType>
    __forceinline bool is_range_okay(SpanType s, OffsetType offset, AccessSizeType access_size)
    {
        //
        // Span size_bytes returns an unsigned value.
        //
        // If offset is signed, casting to unsigned results in it becoming a massive
        // unsigned value.
        //

        return (
            (s.size_bytes() >= static_cast<size_t>(access_size)) &&
            (s.size_bytes() - static_cast<size_t>(access_size) >= static_cast<size_t>(offset)));
    }

    //
    // Helper function, do not use outside of this header.
    //
    // A performant bounds check that ensures that it is valid to dereference type T
    // from &span[offset].
    //

    template <class T, class SpanType, class OffsetType>
    __forceinline bool is_range_okay(SpanType s, OffsetType offset)
    {
        return is_range_okay(s, offset, sizeof(T));
    }
} // namespace details

template <class T>
struct CanAliasStorage
    : public std::bool_constant<
          std::is_same<T, gsl::byte>::value || std::is_same<T, const gsl::byte>::value || std::is_same<T, unsigned char>::value ||
          std::is_same<T, const unsigned char>::value || std::is_same<T, signed char>::value ||
          std::is_same<T, const signed char>::value || std::is_same<T, char>::value || std::is_same<T, const char>::value>
{
};

//
// get_struct is used to safely retrieve a pointer to a structure contained
// within a span. This is useful for tasks such as protocol parsing.
//
// offset denotes the offset in bytes from the start of the span where the
// structure begins.
//

template <class T, class SpanType, class OffsetType>
__forceinline T* get_struct(SpanType s, OffsetType offset)
{
    static_assert(
        CanAliasStorage<typename SpanType::element_type>::value, "You can only call get_struct on a span of bytes or chars.");
    static_assert(is_standard_layout_v<T>, "Your destination type should be standard-layout");

    Expects(details::is_range_okay<T>(s, offset));

    return reinterpret_cast<T*>(s.data() + offset);
}

template <class T, class SpanType>
__forceinline T* get_struct(SpanType s)
{
    return get_struct<T>(s, 0);
}

//
// try_get_struct is used to conditionally retrieve a pointer to a structure
// contained within a span. This is useful for tasks such as protocol parsing.
// This helper differs from get_struct in that it does not failfast in the
// case of a range violation, but rather returns nullptr.
//
// offset denotes the offset in bytes from the start of the span where the
// structure begins.
//

template <class T, class SpanType, class OffsetType>
__forceinline T* try_get_struct(SpanType s, OffsetType offset)
{
    static_assert(
        CanAliasStorage<typename SpanType::element_type>::value, "You can only call try_get_struct on a span of bytes or chars.");
    static_assert(is_standard_layout_v<T>, "Your destination type should be standard-layout");

    if (details::is_range_okay<T>(s, offset))
    {
        return reinterpret_cast<T*>(s.data() + offset);
    }
    else
    {
        return nullptr;
    }
}

template <class T, class SpanType>
__forceinline T* try_get_struct(SpanType s)
{
    return try_get_struct<T>(s, 0);
}

template <class ByteType = gsl::byte, class T>
__forceinline gsl::span<const ByteType> struct_as_bytes(const T& structure)
{
    static_assert(CanAliasStorage<ByteType>::value, "struct_as_bytes may only convert to a span of bytes or chars.");
    static_assert(is_standard_layout_v<T>, "Your input structure type should be standard-layout");

    return {reinterpret_cast<const ByteType*>(&structure), sizeof(structure)};
}

//
// Convert a struct to a writeable span of bytes.
// Accepts alternate single byte types but defaults to gsl::byte.
//

template <class ByteType = gsl::byte, class T>
__forceinline gsl::span<ByteType> struct_as_writeable_bytes(T& structure)
{
    static_assert(CanAliasStorage<ByteType>::value, "struct_as_writeable_bytes may only convert to a span of bytes or chars.");
    static_assert(is_standard_layout_v<T>, "Your input structure type should be standard-layout");

    return {reinterpret_cast<ByteType*>(&structure), sizeof(structure)};
}

template <class NewClass, class CurrentSpan>
__forceinline gsl::span<NewClass> convert_span_truncate(CurrentSpan s)
{
    NewClass* ptr = reinterpret_cast<NewClass*>(s.data());
    return {ptr, s.size_bytes() / sizeof(NewClass)};
}

//
// Convert a span of one type to another type. Throws if
// the new type doesn't fit correctly in the existing span.
//

template <class NewClass, class CurrentSpan>
__forceinline gsl::span<NewClass> convert_span(CurrentSpan s)
{
    Expects(s.size_bytes() % sizeof(NewClass) == 0);

    return convert_span_truncate<NewClass, CurrentSpan>(s);
}
} // namespace gslhelpers
