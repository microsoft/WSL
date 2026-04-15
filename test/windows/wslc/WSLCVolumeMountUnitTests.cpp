/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVolumeMountUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC volume mount argument parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "ContainerModel.h"

using namespace wsl::windows::wslc::models;

namespace WSLCVolumeMount {

class WSLCVolumeMountUnitTests
{
    WSL_TEST_CLASS(WSLCVolumeMountUnitTests)

    TEST_METHOD(VolumeMount_Parse_ReturnExpectedResult)
    {
        // Volume value => host, container, readonly
        std::vector<std::tuple<std::wstring, std::wstring, std::string, bool>> validVolumeArgs = {
            {LR"(C:\hostPath:/containerPath)", LR"(C:\hostPath)", R"(/containerPath)", false},
            {LR"(C:\hostPath:/containerPath:ro)", LR"(C:\hostPath)", R"(/containerPath)", true},
            {LR"(C:\hostPath:/containerPath:rw)", LR"(C:\hostPath)", R"(/containerPath)", false},
            {LR"(C:\host Path:/container Path:ro)", LR"(C:\host Path)", R"(/container Path)", true},
            {LR"(C:\host Path:/container Path:rw)", LR"(C:\host Path)", R"(/container Path)", false},
            {LR"(C:\hostPath::ro)", LR"(C:\hostPath)", R"()", true},
            {LR"(C:\hostPath:)", LR"(C:\hostPath)", R"()", false},
            {LR"(C:\hostPath::rw)", LR"(C:\hostPath)", R"()", false},
            {LR"(C:\hostPath:/containerPath:)", LR"(C:\hostPath:/containerPath)", R"()", false},
            {LR"(C:/hostPath:ro)", LR"(C)", R"(/hostPath)", true},
            {LR"(C:/hostPath)", LR"(C)", R"(/hostPath)", false},
            {LR"(::)", LR"(:)", R"()", false},
        };

        for (const auto& arg : validVolumeArgs)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing volume argument: '{}'", std::get<0>(arg)).c_str());
            auto result = VolumeMount::Parse(std::get<0>(arg));
            VERIFY_ARE_EQUAL(std::get<1>(arg), result.HostPath());
            VERIFY_ARE_EQUAL(std::get<2>(arg), result.ContainerPath());
            VERIFY_ARE_EQUAL(std::get<3>(arg), result.IsReadOnly());
        }
    }

    TEST_METHOD(VolumeMount_Parse_InvalidArgs)
    {
        std::vector<std::wstring> emptyHostPathCases = {
            LR"(:/containerPath)",
            LR"(:/containerPath:ro)",
            LR"(:)",
        };

        for (const auto& value : emptyHostPathCases)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing invalid volume argument: '{}'", value).c_str());
            VERIFY_THROWS(VolumeMount::Parse(value), wil::ResultException);
        }
    }

    TEST_METHOD(VolumeMount_Parse_InvalidContainerPath)
    {
        // Drive colon gets misinterpreted as the host:container separator,
        // producing a non-absolute container path. Caught by container path validation.
        std::vector<std::wstring> invalidPathCases = {
            LR"(C:\hostPath:ro)",                          // host='C', container=\hostPath (not absolute)
            LR"(C:\hostPath)",                             // host='C', container=\hostPath (not absolute)
            LR"(C:\hostPath:/containerPath:invalid_mode)", // container=invalid_mode (not absolute)
            LR"(C:\hostPath:/containerPath:ro:extra)",     // container=extra (not absolute)
        };

        for (const auto& value : invalidPathCases)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing invalid path: '{}'", value).c_str());
            VERIFY_THROWS(VolumeMount::Parse(value), wil::ResultException);
        }
    }
};
} // namespace WSLCVolumeMount