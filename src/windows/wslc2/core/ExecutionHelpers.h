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

namespace wsl::windows::wslc
{
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
    };

    // A callback function that can be used for logging map actions.
    template <typename Enum>
    using EnumBasedVariantMapActionCallback = void (*)(const void* map, Enum value, EnumBasedVariantMapAction action);

    // Provides a map of the Enum to the mapped types.
    template <typename Enum, template<Enum> typename Mapping, EnumBasedVariantMapActionCallback<Enum> Callback = nullptr>
    struct EnumBasedVariantMap
    {
        using Variant = EnumBasedVariant<Enum, Mapping>;

        template <Enum E>
        using mapping_t = typename Mapping<E>::value_t;

        // Adds a value to the map, or overwrites an existing entry.
        // This must be used to create the initial data entry, but Get can be used to modify.
        template <Enum E>
        void Add(mapping_t<E>&& v)
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Add);
            }
            m_data[E].emplace<Variant::Index(E)>(std::move(std::forward<mapping_t<E>>(v)));
        }

        template <Enum E>
        void Add(const mapping_t<E>& v)
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Add);
            }
            m_data[E].emplace<Variant::Index(E)>(v);
        }

        // Return a value indicating whether the given enum is stored in the map.
        bool Contains(Enum e) const
        {
            if constexpr (Callback)
            {
                Callback(this, e, EnumBasedVariantMapAction::Contains);
            }
            return (m_data.find(e) != m_data.end());
        }

        // Gets the value.
        template <Enum E>
        mapping_t<E>& Get()
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Get);
            }
            return std::get<Variant::Index(E)>(GetVariant(E));
        }

        template <Enum E>
        const mapping_t<E>& Get() const
        {
            if constexpr (Callback)
            {
                Callback(this, E, EnumBasedVariantMapAction::Get);
            }
            return std::get<Variant::Index(E)>(GetVariant(E));
        }

    private:
        typename Variant::variant_t& GetVariant(Enum e)
        {
            auto itr = m_data.find(e);
            THROW_HR_IF_MSG(E_NOT_SET, itr == m_data.end(), "GetVariant(%d)", static_cast<int>(e));
            return itr->second;
        }

        const typename Variant::variant_t& GetVariant(Enum e) const
        {
            auto itr = m_data.find(e);
            THROW_HR_IF_MSG(E_NOT_SET, itr == m_data.cend(), "GetVariant(%d)", static_cast<int>(e));
            return itr->second;
        }

        std::map<Enum, typename Variant::variant_t> m_data;
    };
} // namespace wsl::windows::wslc
    