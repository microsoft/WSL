/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgMap.h

Abstract:

    Declaration of ArgMap, the container for parsed command-line arguments and their validated
    (converted) value cache. Split out from ArgumentTypes.h, which holds only the argument enums
    and type mappings ArgMap is built on.

--*/
#pragma once
#include "ArgumentTypes.h"
#include "EnumVariantMap.h"
#include <any>
#include <map>
#include <set>
#include <type_traits>
#include <vector>
#include <utility>

namespace wsl::windows::wslc::argument {

struct ArgMap;

// Validates one argument on demand against its current raw values. Defined in ArgumentValidation.cpp
// so this header stays decoupled from the converter/domain headers.
void EnsureArgumentValidated(ArgMap& map, ArgType type);

// Map-action callback (defined after ArgMap, as it calls a member): any raw Add/Remove drops that
// ArgType's memoized validation state.
inline void ArgMapInvalidateValidatedCache(const void* map, ArgType type, EnumBasedVariantMapAction action);

// This is the main ArgType map used for storing parsed arguments.
struct ArgMap : wsl::windows::wslc::EnumBasedVariantMap<ArgType, wsl::windows::wslc::argument::details::ArgDataMapping, &ArgMapInvalidateValidatedCache>
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
    void AddValidated(typename details::ArgConvertedTypeMapping<E>::value_t value)
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

    // Drops `type`'s memoized validation state (converted cache and validated record) so it never
    // outlives the raw data. Const: touches only mutable memoized state, not the raw storage.
    void InvalidateValidated(ArgType type) const
    {
        m_validated.erase(type);
        m_validatedTypes.erase(type);
    }

    // Records `type` as validated for its current raw values so reads skip re-validation. Const:
    // updates only mutable memoized state.
    void MarkValidated(ArgType type) const
    {
        m_validatedTypes.insert(type);
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
            // Validate-only arguments have no converted cache but can still fail validation, so run
            // it on demand before returning the raw value (covers values added post-validation).
            EnsureValidated(E);
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
            // See GetValue: ensure validate-only arguments are checked on demand too.
            EnsureValidated(E);
            return GetAll<E>();
        }
        else
        {
            return GetAllValidated<E>();
        }
    }

private:
    // Validates `type` against its current raw values unless already recorded as validated. The
    // record is set by a completed validation and cleared by the map-action callback on any raw
    // Add/Remove, so an argument added or overwritten after the up-front pass is validated on
    // demand, and its errors reported, exactly like a command-line value.
    void EnsureValidated(ArgType type) const
    {
        if (m_validatedTypes.count(type) != 0)
        {
            return;
        }

        // Safe: ArgMap is a mutable member of the execution context, never a genuinely const object,
        // so validating through a non-const reference is well-defined.
        EnsureArgumentValidated(const_cast<ArgMap&>(*this), type);
    }

    // Branch helper for GetValue's converted path. Private so callers go through GetValue.
    template <ArgType E>
    const typename details::ArgConvertedTypeMapping<E>::value_t& GetValidated() const
    {
        using value_t = typename details::ArgConvertedTypeMapping<E>::value_t;
        static_assert(
            !std::is_same_v<value_t, details::NoConversion>,
            "This argument has no converted type (NoConversion); it cannot be read from the cache. "
            "Declare its ConvertedType in ArgumentDefinitions.h to enable caching.");

        // Validate on demand if `E` is not recorded as validated (added or overwritten after the
        // up-front pass), so the value read here is converted and its errors reported as usual.
        EnsureValidated(E);

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

        // See GetValidated: validate on demand if `E`'s validated record was cleared post-validation.
        EnsureValidated(E);

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

    // ArgTypes validated against their current raw values. Distinct from m_validated (only converted
    // arguments populate that), so validate-only arguments are covered too. Cleared per type by
    // InvalidateValidated on a raw Add/Remove.
    mutable std::set<ArgType> m_validatedTypes;
};

// Only raw mutations invalidate; reads are ignored. We touch only ArgMap's mutable validation state,
// so the base's const-ness is honored. The base subobject is at offset 0 of ArgMap, so recovering
// the ArgMap pointer from `map` is valid.
inline void ArgMapInvalidateValidatedCache(const void* map, ArgType type, EnumBasedVariantMapAction action)
{
    if (action == EnumBasedVariantMapAction::Add || action == EnumBasedVariantMapAction::Remove)
    {
        static_cast<const ArgMap*>(map)->InvalidateValidated(type);
    }
}

} // namespace wsl::windows::wslc::argument
