// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "EnumVariantMap.h"
#include <string>
#include <vector>
#include <array>
#include <type_traits>

namespace wsl::windows::wslc::argument
{
    // Generate ArgType enum from X-macro
    enum class ArgType : size_t
    {
#define WSLC_ARG_ENUM(EnumName, Name, Alias, Desc, DataType, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet) \
        EnumName,

#include "ArgumentDefinitions.h"
        WSLC_ARGUMENTS(WSLC_ARG_ENUM)
#undef WSLC_ARG_ENUM

        // This should always be at the end
        Max,
    };

    namespace details
    {
        template <ArgType D>
        struct ArgDataMapping
        {
        };

        // Helper macro to remove parentheses from a type
#define WSLC_REMOVE_PARENS(...) __VA_ARGS__

        // Generate data mappings from X-macro
#define WSLC_ARG_MAPPING(EnumName, Name, Alias, Desc, DataType, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet) \
        template<> \
        struct ArgDataMapping<ArgType::EnumName> \
        { \
            using value_t = WSLC_REMOVE_PARENS DataType; \
        };

        WSLC_ARGUMENTS(WSLC_ARG_MAPPING)
#undef WSLC_ARG_MAPPING
#undef WSLC_REMOVE_PARENS

    }

    // Describes the value type of the argument, which determines how it is parsed.
    enum class ValueType
    {
        Bool,
        String,
        StringSet,
    };

    namespace details
    {
        // Type trait to automatically deduce ArgumentType from the data type
        template <typename T>
        struct ArgumentTypeDeducer
        {
            // Default case - shouldn't be hit
            static constexpr ValueType value = ValueType::String;
        };

        // Specialization for bool -> Bool
        template <>
        struct ArgumentTypeDeducer<bool>
        {
            static constexpr ValueType value = ValueType::Bool;
        };

        // Specialization for std::wstring -> String
        template <>
        struct ArgumentTypeDeducer<std::wstring>
        {
            static constexpr ValueType value = ValueType::String;
        };

        // Specialization for std::vector<std::wstring> -> StringSet
        template <>
        struct ArgumentTypeDeducer<std::vector<std::wstring>>
        {
            static constexpr ValueType value = ValueType::StringSet;
        };

        // Helper to get ArgumentType from ArgType at compile time
        template <ArgType A>
        constexpr ValueType GetValueType()
        {
            using DataType = typename ArgDataMapping<A>::value_t;
            return ArgumentTypeDeducer<DataType>::value;
        }

        // Compile-time generation of lookup table using template recursion
        template <size_t... Is>
        constexpr auto MakeArgValueTypeLookupTable(std::index_sequence<Is...>)
        {
            return std::array<ValueType, sizeof...(Is)>{
                GetValueType<static_cast<ArgType>(Is)>()...
            };
        }

        // The compile-time generated lookup table
        inline constexpr auto ArgValueTypeLookupTable = 
            MakeArgValueTypeLookupTable(std::make_index_sequence<static_cast<size_t>(ArgType::Max)>());
    }

    struct Args : wsl::windows::wslc::EnumBasedVariantMap<ArgType, wsl::windows::wslc::argument::details::ArgDataMapping>
    {
        // O(1) runtime lookup with no switch statement needed
        static constexpr ValueType GetValueType(ArgType argType)
        {
            return details::ArgValueTypeLookupTable[static_cast<size_t>(argType)];
        }

        // Compile-time version for when ArgType is known at compile time
        template <ArgType A>
        static constexpr ValueType GetValueType()
        {
            return details::GetValueType<A>();
        }
    };

    // General format:  commandname [Standard|Bool]* [Positional|PositionalGroup] [ForwardGroup]
    enum class Kind
    {
        // Argument is a flag or a value specified with the argument name.
        Standard,

        // Argument is implied by the absence of a name or specifier, and determines
        // which args are standard and which args are forwarded.
        Positional,

        // Argument is intended to represent one or more arguments that are forwarded.
        // to another program or command.
        Forward,
    };

    // Categories an arg type can belong to.
    // Used to reason about the arguments present without having to repeat the same
    // lists every time.
    enum class Category : uint32_t
    {
        None = 0x0,
    };

    DEFINE_ENUM_FLAG_OPERATORS(Category);

    // Exclusive sets an argument can belong to.
    // Only one argument from each exclusive set is allowed at a time.
    enum class ExclusiveSet : uint32_t
    {
        None = 0x0,
        Max = 0x1, // This should always be at the end; used for validation
    };

    DEFINE_ENUM_FLAG_OPERATORS(ExclusiveSet);

    // Controls the visibility of the field.
    enum class Visibility
    {
        // Shown in the example.
        Example,
        // Shown only in the table below the example.
        Help,
        // Not shown in help.
        Hidden,
    };

    // Defines an argument with no alias.
    constexpr static wchar_t NoAlias = L'\0';
}
