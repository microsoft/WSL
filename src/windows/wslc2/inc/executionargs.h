// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <string>
#include <string_view>
#include <map>
#include <vector>

namespace wsl::windows::wslc
{
    struct Args
    {
        enum class Type : uint32_t
        {
            Info,           // About WSLC CLI
            Help,           // Display help information
            SessionId,      // Session ID
            TestArg,        // Argument for testing purposes

            // Container
            Attach,         // Attach
            Interactive,    // Interactive
            ContainerId,    // Container ID

            // This should always be at the end
            Max
        };

        template<typename... T, std::enable_if_t<(... && std::is_same_v<T, Args::Type>), bool> = true>
        bool Contains(T... arg) const
        {
            return (... && (m_parsedArgs.count(arg) != 0));
        }

        const std::vector<std::wstring>* GetArgs(Type arg) const
        {
            auto itr = m_parsedArgs.find(arg);
            return (itr == m_parsedArgs.end() ? nullptr : &(itr->second));
        }

        std::wstring_view GetArg(Type arg) const
        {
            auto itr = m_parsedArgs.find(arg);

            if (itr == m_parsedArgs.end())
            {
                return {};
            }

            return itr->second[0];
        }

        size_t GetCount(Type arg) const
        {
            auto args = GetArgs(arg);
            return (args ? args->size() : 0);
        }

        bool AddArg(Type arg)
        {
            return m_parsedArgs[arg].empty();
        }

        void AddArg(Type arg, std::wstring value)
        {
            m_parsedArgs[arg].emplace_back(std::move(value));
        }

        void AddArg(Type arg, std::wstring_view value)
        {
            m_parsedArgs[arg].emplace_back(value);
        }

        bool Empty()
        {
            return m_parsedArgs.empty();
        }

        size_t GetArgsCount() const
        {
            return m_parsedArgs.size();
        }

        std::vector<Type> GetTypes() const
        {
            std::vector<Type> types;

            for (auto const& i : m_parsedArgs)
            {
                types.emplace_back(i.first);
            }

            return types;
        }

    private:
        std::map<Type, std::vector<std::wstring>> m_parsedArgs;
    };
}
