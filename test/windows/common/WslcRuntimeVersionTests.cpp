// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "WslcsdkPrivate.h"

namespace WslcRuntimeVersionTests {
class WslcRuntimeVersionTests
{
    WSL_TEST_CLASS(WslcRuntimeVersionTests)

    TEST_METHOD(FeatureVersionRequirement)
    {
        for (const WSLCCompatVersion version :
             {WSLCCompatVersion{2, 9, 5}, WSLCCompatVersion{3, 0, 2}, WSLCCompatVersion{3, 0, 5}, WSLCCompatVersion{3, 0, 9}})
        {
            if (version.Major == WSL_PACKAGE_VERSION_MAJOR && version.Minor == WSL_PACKAGE_VERSION_MINOR && version.Revision == WSL_PACKAGE_VERSION_REVISION)
            {
                continue;
            }

            wil::unique_cotaskmem_string message;
            VERIFY_ARE_EQUAL(WSLC_E_RUNTIME_UPDATE_NEEDED, CheckWslRuntimeVersionSupport(version, c_wslcFeatureMinimumVersion, &message));
            VERIFY_IS_NOT_NULL(message.get());
            VERIFY_ARE_EQUAL(
                wsl::shared::Localization::MessageWslcOperationRequiresWslVersion(
                    std::format("{}.{}.{}", version.Major, version.Minor, version.Revision), "3.1.0"),
                std::wstring(message.get()));
            VERIFY_ARE_EQUAL(WSLC_E_RUNTIME_UPDATE_NEEDED, CheckWslRuntimeVersionSupport(version, c_wslcFeatureMinimumVersion, nullptr));
        }

        for (const WSLCCompatVersion version : {WSLCCompatVersion{3, 1, 0}, WSLCCompatVersion{3, 1, 1}, WSLCCompatVersion{4, 0, 0}})
        {
            wil::unique_cotaskmem_string message;
            VERIFY_SUCCEEDED(CheckWslRuntimeVersionSupport(version, c_wslcFeatureMinimumVersion, &message));
            VERIFY_IS_NULL(message.get());
        }
    }

    TEST_METHOD(MatchingDevelopmentRuntime)
    {
        const WSLCCompatVersion version{WSL_PACKAGE_VERSION_MAJOR, WSL_PACKAGE_VERSION_MINOR, WSL_PACKAGE_VERSION_REVISION};
        wil::unique_cotaskmem_string message;
        VERIFY_SUCCEEDED(CheckWslRuntimeVersionSupport(version, {UINT32_MAX, UINT32_MAX, UINT32_MAX}, &message));
        VERIFY_IS_NULL(message.get());
    }
};
} // namespace WslcRuntimeVersionTests
