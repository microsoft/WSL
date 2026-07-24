/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentTypes.h

Abstract:

    Declaration of the ArgumentTypes, which includes all ArgTypes and their properties.

--*/
#pragma once
#include "ArgumentDefinitions.h"
#include "EnumVariantMap.h"
#include <string>
#include <vector>
#include <array>
#include <type_traits>

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
    // overwrites the earlier one rather than being an error. For flags, "present" always
    // means true, so a later "--flag=false" clears it.
    Single,

    // Accepts any number of values, which accumulate (e.g. --publish, --env).
    Unlimited,
};

// Generate ArgType enum from X-macro
enum class ArgType : size_t
{
#define WSLC_ARG_ENUM(EnumName, Name, Alias, Kind, Desc) EnumName,
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
#define WSLC_ARG_MAPPING(EnumName, Name, Alias, ArgumentKind, Desc) \
    template <> \
    struct ArgDataMapping<ArgType::EnumName> \
    { \
        using value_t = typename KindToType<ArgumentKind>::type; \
    };

    WSLC_ARGUMENTS(WSLC_ARG_MAPPING)
#undef WSLC_ARG_MAPPING

} // namespace details

// This is the main ArgType map used for storing parsed arguments.
struct ArgMap : wsl::windows::wslc::EnumBasedVariantMap<ArgType, wsl::windows::wslc::argument::details::ArgDataMapping>
{
    // Reads a boolean (Kind::Flag) argument's effective value in one call. A flag stores its
    // explicit parsed value when specified (docker-style "--flag"/"--flag=true" => true,
    // "--flag=false" => false) and is absent when not specified. Prefer this over a bare
    // Contains() for flags: Contains() only tells you the flag was seen, while GetFlag() folds
    // the presence check and the stored value into a single "is this flag effectively on?" test.
    //
    //   if (args.GetFlag<ArgType::Quiet>()) { ... }              // default-off flag
    //   bool removeOnExit = args.GetFlag<ArgType::Remove>(true); // default-on flag; --rm=false disables
    //
    // defaultValue is returned when the flag was not specified; pass true for flags whose
    // behavior is on by default and must be turned off with "--flag=false".
    template <ArgType E>
    bool GetFlag(bool defaultValue = false) const
    {
        static_assert(std::is_same_v<mapping_t<E>, bool>, "GetFlag is only valid for Kind::Flag arguments");
        return Contains(E) ? Get<E>() : defaultValue;
    }
};

} // namespace wsl::windows::wslc::argument
