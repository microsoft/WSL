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
#include <any>
#include <map>
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

    // Validated-value cache. Argument validation converts raw strings into typed values and caches
    // them here so execution reuses them without re-parsing. The store is type-erased (std::any keyed
    // by ArgType) to keep this base header free of the domain headers that define the converted types;
    // access is by a compile-time ArgType whose value type is derived from the argument's ConvertedType
    // (ArgumentConvertedTypes.h), so a wrong-type access is a compile error. A multimap preserves order
    // and multiplicity for arguments that allow multiple values.
    template <ArgType E>
    void AddValidated(typename details::ArgConvertedTypeMapping<E>::value_t value) const
    {
        using value_t = typename details::ArgConvertedTypeMapping<E>::value_t;
        static_assert(
            !std::is_same_v<value_t, details::NoConversion>,
            "This argument has no converted type (NoConversion); it cannot be cached. "
            "Declare its ConvertedType in ArgumentDefinitions.h to enable caching.");

        m_validated.emplace(E, std::any{std::move(value)});
    }

    bool ContainsValidated(ArgType type) const
    {
        return m_validated.find(type) != m_validated.end();
    }

    size_t CountValidated(ArgType type) const
    {
        return m_validated.count(type);
    }

    // Reads a value argument in one call: the cached converted value if the argument declares a
    // ConvertedType, otherwise the raw parsed value. Valid for Kind::Value/Positional/Forward; using
    // it on a Kind::Flag is a compile error (use GetFlag).
    template <ArgType E>
    decltype(auto) GetValue() const
    {
        static_assert(
            !std::is_same_v<mapping_t<E>, bool>,
            "GetValue is for Kind::Value/Positional/Forward arguments; use GetFlag for Kind::Flag arguments.");

        if constexpr (std::is_same_v<typename details::ArgConvertedTypeMapping<E>::value_t, details::NoConversion>)
        {
            return Get<E>();
        }
        else
        {
            return GetValidated<E>();
        }
    }

    // Like GetValue, but returns every value for an argument that may appear multiple times (ArgMap is
    // a multimap), in insertion order.
    template <ArgType E>
    auto GetAllValues() const
    {
        static_assert(
            !std::is_same_v<mapping_t<E>, bool>,
            "GetAllValues is for Kind::Value/Positional/Forward arguments; use GetFlag for Kind::Flag arguments.");

        if constexpr (std::is_same_v<typename details::ArgConvertedTypeMapping<E>::value_t, details::NoConversion>)
        {
            return GetAll<E>();
        }
        else
        {
            return GetAllValidated<E>();
        }
    }

private:
    // Branch helper for GetValue's converted path. Private so callers go through GetValue.
    template <ArgType E>
    const typename details::ArgConvertedTypeMapping<E>::value_t& GetValidated() const
    {
        using value_t = typename details::ArgConvertedTypeMapping<E>::value_t;
        static_assert(
            !std::is_same_v<value_t, details::NoConversion>,
            "This argument has no converted type (NoConversion); it cannot be read from the cache. "
            "Declare its ConvertedType in ArgumentDefinitions.h to enable caching.");

        // The cache is populated once during validation; a count mismatch means it is stale (e.g. an
        // argument was added afterward) and execution would read the wrong data.
        WI_ASSERT_MSG(Count(E) == CountValidated(E), "validated cache is stale: argument count does not match validated count");

        auto itr = m_validated.find(E);
        THROW_HR_IF_MSG(E_NOT_SET, itr == m_validated.end(), "GetValidated(%d): argument not validated", static_cast<int>(E));

        // any_cast cannot fail: entries under key E are only ever written by AddValidated<E>, which
        // stores exactly value_t. A null result is an internal invariant violation, not a runtime case.
        const value_t* value = std::any_cast<value_t>(&itr->second);
        WI_ASSERT_MSG(value != nullptr, "validated cache holds the wrong type for this argument");

        return *value;
    }

    // Branch helper for GetAllValues's converted path. Private so callers go through GetAllValues.
    template <ArgType E>
    std::vector<typename details::ArgConvertedTypeMapping<E>::value_t> GetAllValidated() const
    {
        using value_t = typename details::ArgConvertedTypeMapping<E>::value_t;
        static_assert(
            !std::is_same_v<value_t, details::NoConversion>,
            "This argument has no converted type (NoConversion); it cannot be read from the cache. "
            "Declare its ConvertedType in ArgumentDefinitions.h to enable caching.");

        // Debug canary: see GetValidated. The cached count must still match the raw argument count.
        WI_ASSERT_MSG(Count(E) == CountValidated(E), "validated cache is stale: argument count does not match validated count");

        std::vector<value_t> results;
        auto range = m_validated.equal_range(E);
        for (auto it = range.first; it != range.second; ++it)
        {
            // See GetValidated: any_cast cannot fail for a correctly populated cache.
            const value_t* value = std::any_cast<value_t>(&it->second);
            WI_ASSERT_MSG(value != nullptr, "validated cache holds the wrong type for this argument");
            results.push_back(*value);
        }

        return results;
    }

    mutable std::multimap<ArgType, std::any> m_validated;
};

} // namespace wsl::windows::wslc::argument
