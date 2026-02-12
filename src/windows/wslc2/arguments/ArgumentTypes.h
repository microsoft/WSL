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
    // This file defines the ArgTpes, the data types of the args, conversion and deduction of types, 
    // and additional argument properties, such as Visibility and Kind.
    // The actual ArgType definitions are in ArgumentDefinitions.h, which is used to generate the enum and mappings.
    // The template at the bottom of this file validates the ArgumentDefinitions at compile time and will
    // produce a compile error if any of the static_asserts fail, with the enum value included in the error message.

    // Generate ArgType enum from X-macro
    enum class ArgType : size_t
    {
#define WSLC_ARG_ENUM(EnumName, Name, Alias, Kind, DataType, Desc) \
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
#define WSLC_ARG_MAPPING(EnumName, Name, Alias, Kind, DataType, Desc) \
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

    // Controls the visibility of the field.
    enum class Visibility
    {
        // Visible in help and also shown in the usage string.
        Usage,

        // Visible in help.
        Help,

        // Not shown in help. The argument is still present and functional.
        Hidden,
    };

    // Defines an argument with no alias.
    constexpr static wchar_t NoAlias = L'\0';
}

// Compile-time validation of the argument table in ArgumentDefinitions.h.
namespace wsl::windows::wslc::argument::validation {

    // Helper to check if a type is a vector.
    template <typename T>
    struct is_vector : std::false_type {};

    template <typename T>
    struct is_vector<std::vector<T>> : std::true_type {};

    template <typename T>
    inline constexpr bool is_vector_v = is_vector<T>::value;

    // Forward declare a type name holder that will be specialized per enum.
    template <ArgType A>
    struct ArgTypeName;

    // Validation: Forward arguments must be vector types.
    template <typename T, Kind K, ArgType A>
    constexpr bool ValidateForwardKind()
    {
        // Force instantiation of ArgTypeName<A> to show the enum value in error.
        using FailedArgument = ArgTypeName<A>;
        static_assert(K != Kind::Forward || is_vector_v<T>,
            "Arguments with Kind::Forward must have a vector data type (e.g., std::vector<std::wstring>). "
            "Check the template instantiation for ArgTypeName<ArgType::YourEnumName> to see which argument failed.");
        return true;
    }

    // Validation: Vector types should be Kind::Forward
    template <typename T, Kind K, ArgType A>
    constexpr bool ValidateVectorUsage()
    {
        // Force instantiation of ArgTypeName<A> to show the enum value in error.
        using FailedArgument = ArgTypeName<A>;
        static_assert(!is_vector_v<T> || K == Kind::Forward,
            "Vector data types must be Kind::Forward. "
            "Check the template instantiation for ArgTypeName<ArgType::YourEnumName> to see which argument failed.");
        return true;
    }

    // Master validation function
    template <typename T, Kind K, ArgType A>
    constexpr bool ValidateArgument()
    {
        return ValidateForwardKind<T, K, A>() && 
               ValidateVectorUsage<T, K, A>();
    }

    // Helper macro to remove parentheses from a type
    #define WSLC_REMOVE_PARENS(...) __VA_ARGS__

    #include "ArgumentDefinitions.h"

    // Macro to generate validation instances for each argument.
    // Also creates a specialized ArgTypeName for better error messages.
    #define VALIDATE_ARGUMENT(EnumName, Name, Alias, Kind, DataType, Desc) \
        template<> struct ArgTypeName<ArgType::EnumName> { static constexpr auto name = #EnumName; }; \
        inline constexpr bool validate_##EnumName = ValidateArgument<WSLC_REMOVE_PARENS DataType, Kind, ArgType::EnumName>();

    // Trigger validation for all arguments at compile time
    WSLC_ARGUMENTS(VALIDATE_ARGUMENT)

    #undef VALIDATE_ARGUMENT
    #undef WSLC_REMOVE_PARENS
} // namespace wsl::windows::wslc::argument::validation