/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Localization.cpp

Abstract:

    This file contains the class to format localized strings.

--*/

#include "precomp.h"
#include "Localization.h"

extern bool g_runningInService;

namespace {

std::vector<std::wstring> GetUserLanguagesImpl()
{
    DWORD count{};
    DWORD bufferSize{};
    GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &bufferSize);

    std::vector<wchar_t> buffer(bufferSize, '\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &bufferSize))
    {
        LOG_LAST_ERROR_MSG("GetUserDefaultLocaleName failed");
        return {L""};
    }

    std::vector<std::wstring> languages;
    for (size_t i = 0; buffer[i] != '\0'; i += wcslen(&buffer[i]) + 1)
    {
        languages.emplace_back(&buffer[i]);
    }

    return languages;
}

std::vector<std::wstring> GetUserLanguages(bool impersonate)
{
    if (g_runningInService)
    {
        // N.B. If we're in the service, the locale needs to be queried every time since different users
        // can have different language configurations.
        std::optional<wil::unique_coreverttoself_call> revert;
        if (impersonate)
        {
            // If we're running in wslservice.exe, impersonation is needed to get the correct locale
            try
            {
                revert = wil::CoImpersonateClient();
            }
            catch (...)
            {
                // Continue if this failed so we fall back to the machine's locale
                LOG_CAUGHT_EXCEPTION();
            }
        }

        return GetUserLanguagesImpl();
    }
    else
    {
        static std::vector<std::wstring> languages;
        static std::once_flag flag;
        std::call_once(flag, [&]() { languages = GetUserLanguagesImpl(); });

        return languages;
    }
}
} // namespace

LPCWSTR wsl::shared::Localization::LookupString(const std::vector<std::pair<std::wstring, LPCWSTR>>& strings, Options options)
{
    WI_ASSERT(!strings.empty());

    try
    {
        for (const auto& language : GetUserLanguages(options != Options::DontImpersonate && g_runningInService))
        {
            for (const auto& e : strings)
            {
                if (e.first == language)
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
