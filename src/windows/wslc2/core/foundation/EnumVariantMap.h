// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Logging.h"

namespace wsl::windows::wslc
{
    // Get the integral value for an enum.
    template <typename E>
    constexpr inline std::enable_if_t<std::is_enum_v<E>, std::underlying_type_t<E>> ToIntegral(E e)
    {
        return static_cast<std::underlying_type_t<E>>(e);
    }

    // Get the enum value for an integral.
    template <typename E>
    constexpr inline std::enable_if_t<std::is_enum_v<E>, E> ToEnum(std::underlying_type_t<E> ut)
    {
        return static_cast<E>(ut);
    }

    // Enum based variant helper.
    // Enum must be an enum whose first member has the value 0, each subsequent member increases by 1, and the final member is named Max.
    // Mapping is a template type that takes one template parameter of type Enum, and whose members define value_t as the type for that enum value.
    template <typename Enum, template<Enum> typename Mapping>
    struct EnumBasedVariant
    {
    private:
        // Used to deduce the variant type; making a variant that includes std::monostate and all Mapping types.
        template <size_t... I>
        static inline auto Deduce(std::index_sequence<I...>) { return std::variant<std::monostate, typename Mapping<static_cast<Enum>(I)>::value_t...>{}; }

    public:
        // Holds data of any type listed in Mapping.
        using variant_t = decltype(Deduce(std::make_index_sequence<static_cast<size_t>(Enum::Max)>()));

        // Gets the index into the variant for the given Data.
        static constexpr inline size_t Index(Enum e) { return static_cast<size_t>(e) + 1; }
    };

    // An action that can be taken on an EnumBasedVariantMap.
    enum class EnumBasedVariantMapAction
    {
        Add,
        Contains,
        Get,
        GetAll,
        Set,
        Count,
        Remove,
        RemoveOne,
    };

    // A callback function that can be used for logging map actions.
    template <typename Enum>
    using EnumBasedVariantMapActionCallback = void (*)(const void* map, Enum value, EnumBasedVariantMapAction action);

    // Forward declaration for EnumBasedVariantMapEmplacer
    template <typename Enum, template<Enum> typename Mapping, typename V>
    struct EnumBasedVariantMapEmplacer;

    // Provides a multimap of the Enum to the mapped types (allows multiple values per key).
    template <typename Enum, template<Enum> typename Mapping, EnumBasedVariantMapActionCallback<Enum> Callback = nullptr>
    struct EnumBasedVariantMap
    {
        using Variant = EnumBasedVariant<Enum, Mapping>;

        template <Enum E>
        using mapping_t = typename Mapping<E>::value_t;

        // Adds a value to the map. With multimap, this always adds a new entry (doesn't overwrite).
        template <Enum E>
        void Add(mapping_t<E>&& v)
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Add);
            }

            // Compile-time type checking - this should always pass since mapping_t<E> is the correct type
            using CleanV = std::remove_cvref_t<mapping_t<E>>;
            static_assert(std::is_same_v<CleanV, mapping_t<E>>, "Type mismatch in Add: provided type does not match the expected type for this enum value");
            
            typename Variant::variant_t variant;
            variant.template emplace<Variant::Index(E)>(std::move(v));
            m_data.emplace(E, std::move(variant));
        }

        template <Enum E>
        void Add(const mapping_t<E>& v)
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Add);
            }

            // Compile-time type checking - this should always pass since mapping_t<E> is the correct type
            using CleanV = std::remove_cvref_t<mapping_t<E>>;
            static_assert(std::is_same_v<CleanV, mapping_t<E>>, "Type mismatch in Add: provided type does not match the expected type for this enum value");

            typename Variant::variant_t variant;
            variant.template emplace<Variant::Index(E)>(v);
            m_data.emplace(E, std::move(variant));
        }

        // Runtime version of Add that takes the enum as a parameter.
        template <typename V>
        void Add(Enum e, V&& v)
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::Add);
            }

            // Check if the type matches the SPECIFIC enum value at compile time if possible
            using CleanV = std::remove_cvref_t<V>;
            
            WSLC_LOG(Debug, Verbose, << L"Adding value to enum " << ToIntegral(e)
                 << L" with type: " << typeid(V).name());

            // Pre-check if this type matches the specific enum value being added to
            if (!IsMatchingType<CleanV>(e))
            {
                WSLC_LOG(Fail, Error, << L"Type mismatch: Type " << typeid(CleanV).name()
                     << L" does not match the expected type for enum value " << ToIntegral(e));
                THROW_HR_MSG(E_INVALIDARG, "Type mismatch: provided type does not match the expected type for enum value %d", static_cast<int>(e));
            }

            typename Variant::variant_t variant;
            EmplaceAtRuntimeIndex(variant, e, std::forward<V>(v), std::make_index_sequence<static_cast<size_t>(Enum::Max)>());
            m_data.emplace(e, std::move(variant));
        }

        // Sets a value in the map, replacing ALL existing entries for the key.
        // Ensures exactly one entry for the key.
        template <Enum E>
        void Set(mapping_t<E>&& v)
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Set);
            }

            m_data.erase(E);
            Add<E>(std::move(v));
        }

        template <Enum E>
        void Set(const mapping_t<E>& v)
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Set);
            }

            m_data.erase(E);
            Add<E>(v);
        }

        // Runtime version of Set
        template <typename V>
        void Set(Enum e, V&& v)
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::Set);
            }

            m_data.erase(e);
            Add(e, std::forward<V>(v));
        }

        // Runtime method to check if value V matches the mapped type for an enum value.
        template <typename V>
        bool IsMatchingType(Enum e) const
        {
            return IsMatchingTypeImpl<V>(e, std::make_index_sequence<static_cast<size_t>(Enum::Max)>());
        }

        // Return a value indicating whether the given enum has at least one entry.
        bool Contains(Enum e) const
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::Contains);
            }
            return (m_data.find(e) != m_data.end());
        }

        // Gets the count of values for a specific enum key.
        size_t Count(Enum e) const
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::Count);
            }
            return m_data.count(e);
        }

        // Gets the FIRST value for the enum key (for backward compatibility).
        // Non-const version returns a reference that can be modified.
        template <Enum E>
        mapping_t<E>& Get()
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Get);
            }
            auto itr = m_data.find(E);
            THROW_HR_IF_MSG(E_NOT_SET, itr == m_data.end(), "Get(%d): key not found", static_cast<int>(E));

            // Validate that the variant holds the expected type at the expected index
            constexpr size_t expectedIndex = Variant::Index(E);
            if (itr->second.index() != expectedIndex)
            {
                WSLC_LOG(Fail, Error, << L"Variant index mismatch for enum " << ToIntegral(E) 
                     << L": expected index " << expectedIndex
                     << L", actual index " << itr->second.index());
                THROW_HR_MSG(E_UNEXPECTED, "Get(%d): variant type mismatch - expected index %zu, got %zu",
                    static_cast<int>(E), expectedIndex, itr->second.index());
            }

            return std::get<expectedIndex>(itr->second);
        }

        // Const overload of Get, cannot be modified.
        template <Enum E>
        const mapping_t<E>& Get() const
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Get);
            }
            auto itr = m_data.find(E);
            THROW_HR_IF_MSG(E_NOT_SET, itr == m_data.cend(), "Get(%d): key not found", static_cast<int>(E));

            // Validate that the variant holds the expected type at the expected index
            constexpr size_t expectedIndex = Variant::Index(E);
            if (itr->second.index() != expectedIndex)
            {
                WSLC_LOG(Fail, Error, << L"Variant index mismatch for enum " << ToIntegral(E)
                     << L": expected index " << expectedIndex
                     << L", actual index " << itr->second.index());
                THROW_HR_MSG(E_UNEXPECTED, "Get(%d): variant type mismatch - expected index %zu, got %zu",
                    static_cast<int>(E), expectedIndex, itr->second.index());
            }

            return std::get<expectedIndex>(itr->second);
        }

        // Gets ALL values for a specific enum key as a vector.
        template <Enum E>
        std::vector<mapping_t<E>> GetAll() const
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::GetAll);
            }

            std::vector<mapping_t<E>> results;
            auto range = m_data.equal_range(E);

            for (auto it = range.first; it != range.second; ++it)
            {
                results.push_back(std::get<Variant::Index(E)>(it->second));
            }

            return results;
        }

        // Runtime version of GetAll with type extraction.
        template <typename T>
        std::vector<T> GetAll(Enum e) const
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::GetAll);
            }

            std::vector<T> results;
            auto range = m_data.equal_range(e);

            for (auto it = range.first; it != range.second; ++it)
            {
                // Extract the value at the appropriate index based on enum value
                auto extractValue = [&]<size_t Index>() {
                    if (auto* value = std::get_if<Index>(&it->second))
                    {
                        if constexpr (std::is_same_v<T, std::decay_t<decltype(*value)>>)
                        {
                            results.push_back(*value);
                        }
                    }
                };

                // Try to extract at the correct index for this enum value
                size_t index = static_cast<size_t>(e) + 1;
                ExtractAtIndex(extractValue, index, std::make_index_sequence<static_cast<size_t>(Enum::Max)>());
            }

            return results;
        }

        // Removes ALL entries for a specific enum key.
        void Remove(Enum e)
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::Remove);
            }
            m_data.erase(e);
        }

        // Removes the FIRST entry for a specific enum key.
        template <Enum E>
        bool RemoveOne()
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::RemoveOne);
            }

            auto itr = m_data.find(E);
            if (itr != m_data.end())
            {
                m_data.erase(itr);
                return true;
            }
            return false;
        }

        // Runtime version of RemoveOne.
        bool RemoveOne(Enum e)
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::RemoveOne);
            }

            auto itr = m_data.find(e);
            if (itr != m_data.end())
            {
                m_data.erase(itr);
                return true;
            }
            return false;
        }

        // Gets the total number of items stored (across all keys).
        size_t GetCount() const
        {
            return m_data.size();
        }

        // Gets a vector of all UNIQUE enum keys stored in the map.
        std::vector<Enum> GetKeys() const
        {
            std::vector<Enum> keys;
            Enum lastKey = static_cast<Enum>(-1);
            bool first = true;

            for (const auto& pair : m_data)
            {
                if (first || pair.first != lastKey)
                {
                    keys.push_back(pair.first);
                    lastKey = pair.first;
                    first = false;
                }
            }

            return keys;
        }

    private:
        // Helper to implement runtime type checking.
        template <typename V, size_t... I>
        bool IsMatchingTypeImpl(Enum e, std::index_sequence<I...>) const
        {
            bool result = false;
            ((static_cast<size_t>(e) == I ? (result = std::is_same_v<std::remove_cvref_t<V>, mapping_t<static_cast<Enum>(I)>>, true) : false) || ...);
            return result;
        }

        // Helper to extract value at runtime-determined index
        template <typename Func, size_t... I>
        void ExtractAtIndex(Func&& func, size_t index, std::index_sequence<I...>) const
        {
            bool matched = false;
            ((index == I + 1 ? (matched = true, func.template operator()<I + 1>(), 0) : 0), ...);
        }

        // Helper to emplace at runtime-determined index
        template <typename V, size_t... I>
        void EmplaceAtRuntimeIndex(typename Variant::variant_t& variant, Enum e, V&& v, std::index_sequence<I...>)
        {
            size_t index = static_cast<size_t>(e) + 1;
            bool handled = false;

            ([&] {
                if (index == I + 1 && !handled)
                {
                    using Emplacer = wsl::windows::wslc::EnumBasedVariantMapEmplacer<Enum, Mapping, V>;
                    Emplacer::template Emplace<I + 1>(variant, std::forward<V>(v));
                    handled = true;
                }
            }(), ...);

            if (!handled)
            {
                using CleanV = std::remove_cvref_t<V>;
                WSLC_LOG(Fail, Error, << L"Invalid enum value " << ToIntegral(e)
                     << L" or type mismatch. Provided type: " << typeid(CleanV).name());
                THROW_HR_MSG(E_INVALIDARG, "Invalid enum value: %d", static_cast<int>(e));
            }
        }

        std::multimap<Enum, typename Variant::variant_t> m_data;
    };

    // Helper for runtime emplacement into std::variant for EnumBasedVariantMap
    template <typename Enum, template<Enum> typename Mapping, typename V>
    struct EnumBasedVariantMapEmplacer
    {
        template <size_t Index>
        static void Emplace(typename EnumBasedVariant<Enum, Mapping>::variant_t& variant, V&& value)
        {
            using TargetType = typename Mapping<static_cast<Enum>(Index - 1)>::value_t;
            using CleanV = std::remove_cvref_t<V>;

            constexpr bool is_same_type = std::is_same_v<CleanV, TargetType>;
            constexpr bool is_convertible = std::is_convertible_v<CleanV, TargetType>;
            constexpr bool is_constructible = std::is_constructible_v<TargetType, CleanV>;

            if constexpr (is_same_type || is_convertible || is_constructible)
            {
                if constexpr (requires { std::wostream{} << value; })
                {
                    WSLC_LOG(Debug, Verbose, << L"Emplacing value: " << value << L" at index " << Index);
                }
                else
                {
                    WSLC_LOG(Debug, Verbose, << L"Emplacing value of type " << typeid(V).name() << L" at index " << Index);
                }

                variant.template emplace<Index>(std::forward<V>(value));
            }
            else
            {
                WSLC_LOG(Fail, Error, << L"Type mismatch at index " << Index
                     << L": CleanV=" << typeid(CleanV).name()
                     << L", TargetType=" << typeid(TargetType).name()
                     << L", same=" << is_same_type
                     << L", convertible=" << is_convertible
                     << L", constructible=" << is_constructible);

                throw std::runtime_error("Runtime type mismatch: cannot convert value to target type for this enum value");
            }
        }
    };
} // namespace wsl::windows::wslc

// Enable enums to be output generically (as their integral value).
template <typename E>
std::enable_if_t<std::is_enum_v<E>, std::ostream&> operator<<(std::ostream& out, E e)
{
    return out << wsl::windows::wslc::ToIntegral(e);
}