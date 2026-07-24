/*++

Copyright (c) Microsoft Corporation. All rights reserved

Parses .gitconfig-style properties files.

--*/

#pragma once

#include <stdio.h>
#include <string>
#include <functional>
#include <map>
#include <optional>
#include <vector>
#include "stringshared.h"
#include "Localization.h"

#ifdef WIN32

#include <filesystem>
#include "wslservice.h"
#include "ExecutionContext.h"

#endif

struct MemoryString
{
    MemoryString(uint64_t& value) : m_value(value) {};
    uint64_t& m_value;
};

enum class ConfigKeyPresence
{
    Absent,
    Present
};

class ConfigKey
{
public:
    using TParseMethod = std::function<void(const char*, const char*, const wchar_t*, unsigned long)>;
    using TGetValueMethod = std::function<std::wstring()>;

    template <typename TType>
    inline ConfigKey(std::vector<const char*>&& names, TType& outValue, ConfigKeyPresence* presence = nullptr) :
        m_names(std::move(names))
    {
        m_parse = [presence = presence, &outValue](const char* name, const char* value, const wchar_t* filename, unsigned long line) {
            if (ParseImpl(name, value, filename, line, outValue) && presence != nullptr)
            {
                *presence = ConfigKeyPresence::Present;
            }
        };

        m_getValue = [&outValue]() { return GetValueImpl(outValue); };
    }

    template <typename TType>
    inline ConfigKey(const char* name, TType& value, ConfigKeyPresence* presence = nullptr) :
        ConfigKey(std::vector<const char*>{name}, value, presence)
    {
    }

    inline ConfigKey(const char* name, MemoryString outValue, ConfigKeyPresence* presence = nullptr) :
        m_names(std::vector<const char*>{name})
    {
        m_parse = [presence = presence, outValue = outValue](const char* name, const char* value, const wchar_t* filename, unsigned long line) {
            if (ParseImpl(name, value, filename, line, outValue) && presence != nullptr)
            {
                *presence = ConfigKeyPresence::Present;
            }
        };

        m_getValue = [outValue = outValue]() { return GetValueImpl(outValue); };
    }

    inline ConfigKey(const char* name, TParseMethod&& parse) : m_names({name}), m_parse(std::move(parse))
    {
    }

    template <typename TEnum>
    inline ConfigKey(
        std::vector<const char*>&& names,
        const std::map<std::string, TEnum, wsl::shared::string::CaseInsensitiveCompare>& values,
        TEnum& outValue,
        ConfigKeyPresence* presence = nullptr) :
        m_names(std::move(names))
    {
        m_parse = [presence = presence, values = values, &outValue](
                      const char* name, const char* value, const wchar_t* filename, unsigned long line) {
            auto result = ParseEnumString(values, value, name, filename, line);
            if (result.has_value())
            {
                outValue = result.value();
                if (presence != nullptr)
                {
                    *presence = ConfigKeyPresence::Present;
                }
            }
        };

        m_getValue = [&values, &outValue]() { return GetEnumString(values, outValue); };
    }

    template <typename TEnum>
    inline ConfigKey(
        const char* name,
        const std::map<std::string, TEnum, wsl::shared::string::CaseInsensitiveCompare>& values,
        TEnum& outValue,
        ConfigKeyPresence* presence = nullptr) :
        ConfigKey(std::vector<const char*>{name}, values, outValue, presence)
    {
    }

    bool Matches(const char* name) const;
    bool Matches(const char* name, size_t length) const;
    void Parse(const char* name, const char* value, const wchar_t* fileName, unsigned long line);
    const std::vector<const char*>& GetNames() const;
    std::wstring GetValue() const;

    template <typename TEnum>
    static inline std::optional<TEnum> ParseEnumString(
        const std::map<std::string, TEnum, wsl::shared::string::CaseInsensitiveCompare>& mappings,
        const char* value,
        const char* name,
        const wchar_t* fileName,
        unsigned long line)
    {
        auto it = mappings.find(value);
        if (it == mappings.end())
        {
            std::stringstream validValues;
            bool first = true;
            for (const auto& e : mappings)
            {
                if (!first)
                {
                    validValues << ", ";
                }

                first = false;
                validValues << e.first;
            }

            EMIT_USER_WARNING(wsl::shared::Localization::MessageConfigInvalidEnum(value, name, fileName, line, validValues.str().c_str()));
            return {};
        }

        return it->second;
    }

    template <typename TEnum>
    static std::wstring GetEnumString(const std::map<std::string, TEnum, wsl::shared::string::CaseInsensitiveCompare>& mappings, TEnum& value)
    {
        auto it = mappings.begin();
        for (; it != mappings.end(); ++it)
        {
            if (it->second == value)
            {
                break;
            }
        }

        return it == mappings.end() ? L"" : wsl::shared::string::MultiByteToWide(it->first);
    }

private:
    static bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, bool& result);
    static bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, int& result);
    static bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, std::string& result);
    static bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, MemoryString result);
    static bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, std::wstring& result);
    static bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, std::filesystem::path& result);

    static std::wstring GetValueImpl(bool result);
    static std::wstring GetValueImpl(int result);
    static std::wstring GetValueImpl(const std::string& result);
    static std::wstring GetValueImpl(const std::optional<std::string>& result);
    static std::wstring GetValueImpl(const MemoryString& result);
    static std::wstring GetValueImpl(const std::wstring& result);

#ifdef WIN32
    static bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, wsl::shared::string::MacAddress& result);
    static std::wstring GetValueImpl(const wsl::shared::string::MacAddress& result);
#endif

    template <typename T>
    static inline bool ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, std::optional<T>& result)
    {
        T storage{};

        if (ParseImpl(name, value, filePath, fileLine, storage))
        {
            result.emplace(std::move(storage));
            return true;
        }

        return false;
    }

    std::vector<const char*> m_names;
    TParseMethod m_parse;
    TGetValueMethod m_getValue;
    std::optional<std::pair<std::string, unsigned long>> m_parseResult;
};

enum
{
    CFG_SKIP_INVALID_LINES = 0x1,
    CFG_SKIP_UNKNOWN_VALUES = 0x2,
    CFG_DEBUG = 0x80000000,
};

int ParseConfigFile(std::vector<ConfigKey>& keys, FILE* file, int flags, const wchar_t* filePath);
int ParseConfigFile(
    std::vector<ConfigKey>& keys,
    FILE* file,
    int flags,
    const wchar_t* filePath,
    std::wstring& configFileOutput,
    std::optional<ConfigKey> outputKey = std::nullopt,
    bool removeKey = false);