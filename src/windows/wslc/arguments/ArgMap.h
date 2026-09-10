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

namespace details {
    struct RawArgMapAccess;

    template <ArgType E, bool IsFlag = std::is_same_v<typename ArgDataMapping<E>::value_t, bool>>
    struct ArgValueTraits;

    template <ArgType E>
    struct ArgValueTraits<E, true>
    {
        using value_t = typename ArgDataMapping<E>::value_t;
        static constexpr bool Converted = false;
    };

    template <ArgType E>
    struct ArgValueTraits<E, false>
    {
        using converted_t = typename ArgConvertedTypeMapping<E>::value_t;
        using value_t = std::conditional_t<std::is_same_v<converted_t, NoConversion>, typename ArgDataMapping<E>::value_t, converted_t>;
        static constexpr bool Converted = !std::is_same_v<converted_t, NoConversion>;
    };
} // namespace details

// Validates one argument on demand against its current raw values. Defined in ArgumentValidation.cpp
// so this header stays decoupled from the converter/domain headers.
void EnsureArgumentValidated(ArgMap& map, ArgType type);

// Map-action callback (defined after ArgMap, as it calls a member): operations that can mutate raw
// values update that ArgType's validation state.
inline void ArgMapInvalidateValidatedCache(const void* map, ArgType type, EnumBasedVariantMapAction action);

// This is the main ArgType map used for storing parsed arguments.
struct ArgMap : private wsl::windows::wslc::EnumBasedVariantMap<ArgType, wsl::windows::wslc::argument::details::ArgDataMapping, &ArgMapInvalidateValidatedCache>
{
private:
    using Base = wsl::windows::wslc::EnumBasedVariantMap<ArgType, wsl::windows::wslc::argument::details::ArgDataMapping, &ArgMapInvalidateValidatedCache>;

    friend struct details::RawArgMapAccess;

    // Raw reads are implementation details used by validation and the typed accessors below.
    // Callers consume arguments through GetValue/GetAllValues so reads validate and freeze them.
    using Base::Get;
    using Base::GetAll;

public:
    ArgMap() = default;
    ArgMap(const ArgMap&) = default;
    ArgMap(ArgMap&&) = default;
    ArgMap& operator=(const ArgMap&) = delete;
    ArgMap& operator=(ArgMap&&) = delete;

    using Base::Add;
    using Base::Contains;
    using Base::Count;
    using Base::GetCount;
    using Base::GetKeys;
    using Base::IsMatchingType;
    using Base::Remove;

    template <ArgType E>
    using value_t = typename details::ArgValueTraits<E>::value_t;

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

        ThrowIfImmutable(E, "add converted validation data");
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

    bool IsValidated(ArgType type) const
    {
        return m_validatedTypes.count(type) != 0;
    }

    // Drops `type`'s memoized validation state (converted cache and validated record) so it never
    // outlives the raw data.
    void InvalidateValidated(ArgType type)
    {
        ThrowIfImmutable(type, "invalidate cached validation data");
        ClearValidated(type);
    }

    // Records `type` as validated for its current raw values so reads skip re-validation.
    void MarkValidated(ArgType type)
    {
        if (IsValidated(type))
        {
            return;
        }

        ThrowIfImmutable(type, "mark the argument as validated");
        m_validatedTypes.insert(type);
    }

    void HandleMapMutation(ArgType type, EnumBasedVariantMapAction action)
    {
        WI_ASSERT(action == EnumBasedVariantMapAction::Add || action == EnumBasedVariantMapAction::GetMutable || action == EnumBasedVariantMapAction::Remove);

        const char* operation = nullptr;
        switch (action)
        {
        case EnumBasedVariantMapAction::Add:
            operation = "add a raw argument value";
            break;

        case EnumBasedVariantMapAction::GetMutable:
            operation = "get mutable access to a raw argument value";
            break;

        case EnumBasedVariantMapAction::Remove:
            operation = "remove the raw argument values";
            break;

        default:
            WI_ASSERT(false);
            return;
        }

        ThrowIfImmutable(type, operation);
        ClearValidated(type);
    }

    // Reads an argument in one call: the cached converted value if the argument declares a
    // ConvertedType, otherwise the raw parsed value. An absent argument resolves to defaultValue,
    // which defaults to the value type's default constructor. The first resolved default is retained
    // so later reads return the same effective value. Caller-provided defaults are already typed and
    // do not populate the raw map or change Contains(). A successful read makes the argument immutable.
    template <ArgType E>
    const value_t<E>& GetValue(value_t<E> defaultValue = {})
    {
        if (const auto* resolvedDefault = GetResolvedDefault<E>())
        {
            return *resolvedDefault;
        }

        if (!Contains(E))
        {
            auto [itr, inserted] = m_resolvedDefaults.emplace(E, std::any{std::move(defaultValue)});
            WI_ASSERT(inserted);
            MarkImmutable(E);

            const auto* value = std::any_cast<value_t<E>>(&itr->second);
            WI_ASSERT_MSG(value != nullptr, "resolved default holds the wrong type for this argument");
            return *value;
        }

        if constexpr (!details::ArgValueTraits<E>::Converted)
        {
            // Validate-only arguments have no converted cache but can still fail validation, so run
            // it on demand before returning the raw value (covers values added post-validation).
            EnsureValidated(E);
            const auto& value = std::as_const(*this).template Get<E>();
            MarkImmutable(E);
            return value;
        }
        else
        {
            const auto& value = GetValidated<E>();
            MarkImmutable(E);
            return value;
        }
    }

    // Like GetValue, but returns every value for an argument that may appear multiple times (ArgMap
    // is a multimap), in insertion order. An absent argument returns an empty vector.
    template <ArgType E>
    auto GetAllValues()
    {
        static_assert(details::ArgDataMapping<E>::c_kind != Kind::Flag, "GetAllValues is not valid for Kind::Flag arguments.");

        if constexpr (!details::ArgValueTraits<E>::Converted)
        {
            // See GetValue: ensure validate-only arguments are checked on demand too.
            EnsureValidated(E);
            auto values = GetAll<E>();
            MarkImmutable(E);
            return values;
        }
        else
        {
            auto values = GetAllValidated<E>();
            MarkImmutable(E);
            return values;
        }
    }

private:
    // Validates `type` against its current raw values unless already recorded as validated. The
    // record is set by a completed validation and cleared by the map-action callback on any raw
    // Add/Remove, so an argument added or overwritten after the up-front pass is validated on
    // demand, and its errors reported, exactly like a command-line value.
    void EnsureValidated(ArgType type)
    {
        if (IsValidated(type))
        {
            return;
        }

        EnsureArgumentValidated(*this, type);
    }

    // Branch helper for GetValue's converted path. Private so callers go through GetValue.
    template <ArgType E>
    const typename details::ArgConvertedTypeMapping<E>::value_t& GetValidated()
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
    std::vector<typename details::ArgConvertedTypeMapping<E>::value_t> GetAllValidated()
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

    template <ArgType E>
    const value_t<E>* GetResolvedDefault() const
    {
        const auto itr = m_resolvedDefaults.find(E);
        if (itr == m_resolvedDefaults.end())
        {
            return nullptr;
        }

        const auto* value = std::any_cast<value_t<E>>(&itr->second);
        WI_ASSERT_MSG(value != nullptr, "resolved default holds the wrong type for this argument");
        return value;
    }

    void MarkImmutable(ArgType type)
    {
        m_immutableTypes.insert(type);
    }

    void ClearValidated(ArgType type)
    {
        m_validated.erase(type);
        m_validatedTypes.erase(type);
    }

    void ThrowIfImmutable(ArgType type, const char* operation) const
    {
        THROW_HR_IF_MSG(
            E_ILLEGAL_METHOD_CALL,
            m_immutableTypes.count(type) != 0,
            "ArgMap argument %d is immutable because its effective value was already read by GetValue/GetAllValues; attempted to "
            "%hs",
            static_cast<int>(type),
            operation);
    }

    std::multimap<ArgType, std::any> m_validated;
    std::map<ArgType, std::any> m_resolvedDefaults;

    // ArgTypes validated against their current raw values. Distinct from m_validated (only converted
    // arguments populate that), so validate-only arguments are covered too. Cleared per type by
    // InvalidateValidated on a raw Add/Remove.
    std::set<ArgType> m_validatedTypes;

    // A successful GetValue/GetAllValues makes that ArgType's raw and validated data immutable.
    std::set<ArgType> m_immutableTypes;
};

// Only operations that can mutate raw values affect validation state; const reads are ignored.
// Recovering the non-const ArgMap from the callback's type-erased pointer is valid because these
// actions originate from non-const base operations. The base subobject is at offset 0 of ArgMap.
inline void ArgMapInvalidateValidatedCache(const void* map, ArgType type, EnumBasedVariantMapAction action)
{
    if (action == EnumBasedVariantMapAction::Add || action == EnumBasedVariantMapAction::GetMutable || action == EnumBasedVariantMapAction::Remove)
    {
        const_cast<ArgMap*>(static_cast<const ArgMap*>(map))->HandleMapMutation(type, action);
    }
}

} // namespace wsl::windows::wslc::argument
