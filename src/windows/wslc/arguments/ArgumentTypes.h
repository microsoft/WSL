/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentTypes.h

Abstract:

    Declaration of the ArgumentTypes, which includes all ArgTypes and their properties.

--*/
#pragma once
#include "pch.h"
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

// Generate ArgType enum from X-macro
enum class ArgType : size_t
{
#define WSLC_ARG_ENUM(EnumName, Name, Alias, Kind, Desc) EnumName,

#include "ArgumentDefinitions.h"
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
        using type = std::wstring;
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
};

} // namespace wsl::windows::wslc::argument
