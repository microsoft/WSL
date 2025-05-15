/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Localization.cpp

Abstract:

    This file contains the Linux localization logic.

--*/

#include "Localization.h"

std::string FormatLanguage(const char* Input)
{
    // Expected format: en_US.UTF-8

    std::string output(Input);
    auto dot = output.find(".");

    if (dot != std::string::npos && dot != 0)
    {
        output = output.substr(0, dot);
    }

    std::replace(output.begin(), output.end(), '_', '-');

    return output;
}

std::optional<std::string> GetUserLanguage()
{
    for (const auto* e : {"LANGUAGE", "LANG", "LC_ALL"})
    {
        if (const auto* lang = getenv(e))
        {
            return lang;
        }
    }

    return {};
}

const char* wsl::shared::Localization::LookupString(const std::vector<std::pair<std::string, const char*>>& strings, Options options)
{
    try
    {
        static const auto language = GetUserLanguage();
        if (language.has_value())
        {
            for (const auto& e : strings)
            {
                if (wsl::shared::string::IsEqual(e.first, language.value(), true))
                {
                    return e.second;
                }
            }
        }
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
    }

    // Default to English is string is not found (English is always the first entry)
    return strings[0].second;
}
