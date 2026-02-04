// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "Util.h"
#include <wil/token_helpers.h>
#include <winrt/Windows.System.Profile.h>

namespace wsl::windows::wslc::util
{
    std::wstring GetOSVersion()
    {
        using namespace winrt::Windows::System::Profile;
        auto versionInfo = AnalyticsInfo::VersionInfo();

        uint64_t version = std::stoull(versionInfo.DeviceFamilyVersion().c_str());
        uint16_t parts[4];

        for (size_t i = 0; i < ARRAYSIZE(parts); ++i)
        {
            parts[i] = version & 0xFFFF;
            version = version >> 16;
        }

        std::wostringstream strstr;
        strstr << versionInfo.DeviceFamily().c_str() << L" v" << parts[3] << L'.' << parts[2] << L'.' << parts[1] << L'.' << parts[0];
        return strstr.str();
    }

    bool IsRunningAsAdmin()
    {
        return wil::test_token_membership(nullptr, SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);
    }
}
