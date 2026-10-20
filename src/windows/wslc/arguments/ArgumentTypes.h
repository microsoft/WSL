/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentTypes.h

Abstract:

    Declaration of the ArgumentTypes, which includes all ArgTypes and their properties.

--*/
#pragma once
#include "ArgumentDefinitions.h"
#include <string>
#include <vector>
#include <array>
#include <type_traits>
#include <utility>

namespace wsl::windows::wslc::argument {
// General format:  commandname [Flag | Value]* [Positional]* [Forward]
// Argument Kind, which determines both parsing behavior and data type.
enum class Kind
{
    // Boolean flag argument (--flag or -f). Data type: bool
    Flag,

    // String value argument (--option value or -o value). Data type: std::wstring
    Value,

    // Positional argument (implied by position, no flag). Data type: std::wstring
    Positional,

    // Forward arguments (remaining args passed through). Data type: std::wstring
    Forward,
};

// How many times an argument may be supplied on a command line.
enum class Limit
{
    // Accepts a single value. Repeats are last-wins (docker-style): a later occurrence
    // overwrites the earlier one rather than being an error. For flags, the stored value
    // is the last one parsed, so "--flag --flag=false" ends up false.
    Single,

    // Accepts any number of values, which accumulate (e.g. --publish, --env).
    Unlimited,
};

// Generate ArgType enum from X-macro
enum class ArgType : size_t
{
#define WSLC_ARG_ENUM(EnumName, Name, Alias, Kind, ConvertedType, Desc) EnumName,
    WSLC_ARGUMENTS(WSLC_ARG_ENUM)
#undef WSLC_ARG_ENUM

    // This should always be at the end
    Max,
};

namespace details {
    // Map Kind to data type
    template <Kind K>
    struct KindToType;

    template <>
    struct KindToType<Kind::Flag>
    {
        using type = bool;
    };

    template <>
    struct KindToType<Kind::Value>
    {
        using type = std::wstring;
    };

    template <>
    struct KindToType<Kind::Positional>
    {
        using type = std::wstring;
    };

    template <>
    struct KindToType<Kind::Forward>
    {
        using type = std::vector<std::wstring>;
    };

    template <ArgType D>
    struct ArgDataMapping
    {
    };

    // Generate data mappings from X-macro - Kind determines the type
#define WSLC_ARG_MAPPING(EnumName, Name, Alias, ArgumentKind, ConvertedType, Desc) \
    template <> \
    struct ArgDataMapping<ArgType::EnumName> \
    { \
        using value_t = typename KindToType<ArgumentKind>::type; \
        static constexpr Kind c_kind = ArgumentKind; \
    };

    WSLC_ARGUMENTS(WSLC_ARG_MAPPING)
#undef WSLC_ARG_MAPPING

    // Sentinel type for arguments that are not converted to a typed value during validation
    // (their raw string is used directly at execution). Arguments mapped to NoConversion cannot
    // be read from or written to the validated cache; doing so is a compile error.
    struct NoConversion
    {
    };

    // Maps an ArgType to the type its string value is converted to during validation. Declared here
    // so ArgMap's cache accessors can name the converted type without depending on the domain headers
    // that define it; the specializations live in ArgumentConvertedTypes.h.
    template <ArgType D>
    struct ArgConvertedTypeMapping;

} // namespace details

} // namespace wsl::windows::wslc::argument
