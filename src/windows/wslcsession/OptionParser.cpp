/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OptionParser.cpp

Abstract:

    Implementation of OptionParser. Handles the non-template error/lookup
    primitives that the templated accessors in OptionParser.h call into.

--*/

#include "precomp.h"
#include "OptionParser.h"

using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

OptionParser::OptionParser(const std::map<std::string, std::string>& Options) noexcept : m_options(Options)
{
}

const std::string* OptionParser::Find(std::string_view Key)
{
    const auto it = m_options.find(std::string(Key));
    if (it == m_options.end())
    {
        return nullptr;
    }

    m_consumed.insert(it->first);
    return &it->second;
}

std::optional<bool> OptionParser::OptionalBool(std::string_view Key)
{
    const auto* value = Find(Key);
    if (value == nullptr)
    {
        return std::nullopt;
    }

    const auto parsed = wsl::shared::string::ParseBool(value->c_str());
    if (!parsed.has_value())
    {
        ThrowInvalid(Key, *value);
    }

    return parsed;
}

void OptionParser::RejectUnknown()
{
    for (const auto& [key, value] : m_options)
    {
        if (m_consumed.find(key) == m_consumed.end())
        {
            ThrowUnknown(key);
        }
    }
}

void OptionParser::ThrowInvalid(std::string_view Key, const std::string& Value)
{
    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcInvalidVolumeOption(std::string(Key), Value));
}

void OptionParser::ThrowMissing(std::string_view Key)
{
    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcMissingVolumeOption(std::string(Key)));
}

void OptionParser::ThrowUnknown(std::string_view Key)
{
    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcUnknownVolumeOption(std::string(Key)));
}

} // namespace wsl::windows::service::wslc
